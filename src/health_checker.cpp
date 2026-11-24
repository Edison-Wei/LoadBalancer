#include "lb/health_checker.hpp"
#include "lb/log.hpp"
#include "lb/proxy_http.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace lb {
namespace {
namespace asio = boost::asio;
namespace http = boost::beast::http;

std::uint64_t timestamp() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}
const char* stateName(BackendState state) {
    switch (state) {
    case BackendState::UNKNOWN:
        return "UNKNOWN";
    case BackendState::HEALTHY:
        return "HEALTHY";
    case BackendState::UNHEALTHY:
        return "UNHEALTHY";
    case BackendState::DRAINING:
        return "DRAINING";
    }
    return "INVALID";
}
const char* reasonName(ProbeReason reason) {
    switch (reason) {
    case ProbeReason::SUCCESS:
        return "success";
    case ProbeReason::HTTP_STATUS:
        return "http_status";
    case ProbeReason::TRANSPORT:
        return "transport";
    case ProbeReason::TIMEOUT:
        return "timeout";
    case ProbeReason::CANCELLED:
        return "cancelled";
    }
    return "invalid";
}
} // namespace

HealthProgress::HealthProgress(unsigned failures, unsigned successes)
    : m_failureThreshold(failures), m_successThreshold(successes) {
    if (!failures || !successes) {
        throw std::invalid_argument("Health thresholds must be positive");
    }
}

BackendState HealthProgress::observe(BackendState current, bool success) {
    if (current == BackendState::DRAINING) {
        return current;
    }
    if (success) {
        m_failures = 0;
        if (m_successes < m_successThreshold) {
            ++m_successes;
        }
        return current == BackendState::UNKNOWN || m_successes >= m_successThreshold
                   ? BackendState::HEALTHY
                   : current;
    }
    m_successes = 0;
    if (m_failures < m_failureThreshold) {
        ++m_failures;
    }
    return m_failures >= m_failureThreshold ? BackendState::UNHEALTHY : current;
}

struct HealthChecker::Entry {
    Entry(asio::any_io_executor executor, BackendPtr value, const HealthOptions& options)
        : backend(std::move(value)), progress(options.failureThreshold, options.successThreshold),
          timer(executor) {}
    BackendPtr backend;
    HealthProgress progress;
    asio::steady_timer timer;
    std::shared_ptr<UpstreamCancellation> active;
    BackendState lastState = BackendState::UNKNOWN;
};

HealthChecker::HealthChecker(asio::any_io_executor executor,
                             std::shared_ptr<BackendRegistry> registry, HealthOptions options,
                             Probe probe, OnError onError)
    : m_executor(std::move(executor)), m_registry(std::move(registry)),
      m_options(std::move(options)), m_probe(std::move(probe)), m_onError(std::move(onError)) {
    validateHealthOptions(m_options);
    if (!m_registry) {
        throw std::invalid_argument("Health checker requires a registry");
    }
    for (auto& backend : m_registry->configuredBackends()) {
        m_entries.push_back(std::make_shared<Entry>(m_executor, backend, m_options));
    }
}

asio::awaitable<ProbeOutcome>
HealthChecker::networkProbe(BackendPtr backend,
                            std::shared_ptr<UpstreamCancellation> cancellation) {
    ProxyOptions options;
    options.connectTimeoutMs = m_options.connectTimeoutMs;
    options.upstreamTimeoutMs = m_options.connectTimeoutMs + m_options.responseTimeoutMs;
    options.maxBodyBytes = 65536;
    options.maxHeaderBytes = 8192;
    ProxyRequest request{http::verb::get, m_options.path, 11};
    prepareUpstreamRequest(request, *backend, "127.0.0.1");
    auto result = co_await forwardToBackend(std::move(request), backend, options,
                                            m_options.responseTimeoutMs, std::move(cancellation));
    if (result.error == UpstreamError::CANCELLED) {
        co_return ProbeOutcome{ProbeReason::CANCELLED, 0};
    }
    if (result.error == UpstreamError::TIMEOUT) {
        co_return ProbeOutcome{ProbeReason::TIMEOUT, 0};
    }
    if (result.error != UpstreamError::NONE) {
        co_return ProbeOutcome{ProbeReason::TRANSPORT, 0};
    }
    co_return ProbeOutcome{result.response.result() == http::status::ok ? ProbeReason::SUCCESS
                                                                        : ProbeReason::HTTP_STATUS,
                           result.response.result_int()};
}

asio::awaitable<void> HealthChecker::probeOnce(const std::shared_ptr<Entry>& entry) {
    const auto before = entry->backend->state();
    if (before != entry->lastState) {
        entry->progress = HealthProgress(m_options.failureThreshold, m_options.successThreshold);
        entry->lastState = before;
    }
    if (m_stopped || before == BackendState::DRAINING ||
        m_registry->findBackend(entry->backend->id) != entry->backend) {
        co_return;
    }
    entry->active = std::make_shared<UpstreamCancellation>();
    const auto started = std::chrono::steady_clock::now();
    const auto result = m_probe ? co_await m_probe(entry->backend, entry->active)
                                : co_await networkProbe(entry->backend, entry->active);
    entry->active.reset();
    if (m_stopped || result.reason == ProbeReason::CANCELLED) {
        co_return;
    }
    auto progress = entry->progress;
    const auto after = progress.observe(before, result.reason == ProbeReason::SUCCESS);
    if (!m_registry->setStateIfCurrent(entry->backend, before, after)) {
        entry->progress = HealthProgress(m_options.failureThreshold, m_options.successThreshold);
        co_return; // stale endpoint/state result never publishes or changes statistics
    }
    entry->progress = progress;
    entry->lastState = after;
    const auto now = timestamp();
    entry->backend->probes.fetch_add(1, std::memory_order_relaxed);
    entry->backend->lastProbeTimeMs.store(now, std::memory_order_relaxed);
    if (result.reason == ProbeReason::SUCCESS) {
        entry->backend->probeSuccesses.fetch_add(1, std::memory_order_relaxed);
    } else {
        entry->backend->probeFailures.fetch_add(1, std::memory_order_relaxed);
        if (result.reason == ProbeReason::TIMEOUT) {
            entry->backend->probeTimeouts.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    LogLine{} << "{\"event\":\"health_probe\",\"timestamp_ms\":" << now << ",\"backend\":\""
              << entry->backend->id << "\",\"reason\":\"" << reasonName(result.reason)
              << "\",\"status\":" << result.status << ",\"duration_ms\":" << elapsed
              << ",\"successes\":" << progress.successes()
              << ",\"failures\":" << progress.failures() << ",\"state\":\"" << stateName(after)
              << "\"}" << std::endl;
    if (before != after) {
        entry->backend->lastTransitionTimeMs.store(now, std::memory_order_relaxed);
        LogLine{} << "{\"event\":\"health_transition\",\"timestamp_ms\":" << now
                  << ",\"backend\":\"" << entry->backend->id << "\",\"from\":\""
                  << stateName(before) << "\",\"to\":\"" << stateName(after) << "\"}" << std::endl;
    }
}

asio::awaitable<void> HealthChecker::bootstrap() {
    if (m_stopped) {
        co_return;
    }
    if (m_bootstrapping || m_bootstrapped) {
        throw std::logic_error("Health bootstrap may only run once");
    }
    m_bootstrapping = true;
    for (const auto& entry : m_entries) {
        co_await probeOnce(entry);
        if (m_stopped) {
            co_return;
        }
    }
    m_bootstrapped = true;
    m_bootstrapping = false;
}

asio::awaitable<void> HealthChecker::periodic(std::shared_ptr<Entry> entry) {
    while (!m_stopped && m_registry->findBackend(entry->backend->id) == entry->backend) {
        entry->timer.expires_after(std::chrono::milliseconds(m_options.intervalMs));
        boost::system::error_code error;
        co_await entry->timer.async_wait(asio::redirect_error(asio::use_awaitable, error));
        if (error || m_stopped) {
            co_return;
        }
        co_await probeOnce(entry);
    }
}

void HealthChecker::start() {
    if (m_started || m_stopped) {
        return;
    }
    if (!m_bootstrapped) {
        throw std::logic_error("Bootstrap must finish before recurring health checks start");
    }
    m_started = true;
    for (const auto& entry : m_entries) {
        auto self = shared_from_this();
        asio::co_spawn(m_executor, periodic(entry), [self](std::exception_ptr error) {
            if (error) {
                self->stop();
                if (self->m_onError) {
                    self->m_onError(error);
                }
            }
        });
    }
}

void HealthChecker::stop() {
    if (m_stopped) {
        return;
    }
    m_stopped = true;
    for (const auto& entry : m_entries) {
        entry->timer.cancel();
        if (entry->active) {
            entry->active->cancel();
        }
    }
    LogLine{} << "{\"event\":\"health_checker_stopped\",\"timestamp_ms\":" << timestamp() << "}"
              << std::endl;
}
} // namespace lb
