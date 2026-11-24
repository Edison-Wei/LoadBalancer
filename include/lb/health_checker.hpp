#pragma once

#include "lb/config.hpp"
#include "lb/registry.hpp"
#include "lb/upstream_proxy.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <exception>
#include <functional>
#include <memory>
#include <vector>

namespace lb {

/// Pure hysteresis state machine. UNKNOWN admits its first successful probe;
/// after exclusion, recovery requires the full success threshold. Counts saturate.
class HealthProgress {
  public:
    HealthProgress(unsigned failures, unsigned successes);
    BackendState observe(BackendState current, bool success);
    [[nodiscard]] unsigned successes() const noexcept { return m_successes; }
    [[nodiscard]] unsigned failures() const noexcept { return m_failures; }

  private:
    unsigned m_failureThreshold;
    unsigned m_successThreshold;
    unsigned m_successes = 0;
    unsigned m_failures = 0;
};

enum class ProbeReason { SUCCESS, HTTP_STATUS, TRANSPORT, TIMEOUT, CANCELLED };
struct ProbeOutcome {
    ProbeReason reason = ProbeReason::SUCCESS;
    unsigned status = 200;
};

/// All calls and probe completions are confined to the runtime control strand.
/// stop() cancels intervals/active probes and gates subsequent publication.
/// Probe injection permits deterministic lifecycle tests without sockets.
class HealthChecker : public std::enable_shared_from_this<HealthChecker> {
  public:
    using Probe = std::function<boost::asio::awaitable<ProbeOutcome>(
        BackendPtr, std::shared_ptr<UpstreamCancellation>)>;
    using OnError = std::function<void(std::exception_ptr)>;

    HealthChecker(boost::asio::any_io_executor executor, std::shared_ptr<BackendRegistry> registry,
                  HealthOptions options, Probe probe = {}, OnError onError = {});
    boost::asio::awaitable<void> bootstrap();
    void start();
    void stop();

  private:
    struct Entry;
    boost::asio::awaitable<void> probeOnce(const std::shared_ptr<Entry>& entry);
    boost::asio::awaitable<void> periodic(std::shared_ptr<Entry> entry);
    boost::asio::awaitable<ProbeOutcome>
    networkProbe(BackendPtr backend, std::shared_ptr<UpstreamCancellation> cancellation);
    boost::asio::any_io_executor m_executor;
    std::shared_ptr<BackendRegistry> m_registry;
    HealthOptions m_options;
    Probe m_probe;
    OnError m_onError;
    std::vector<std::shared_ptr<Entry>> m_entries;
    bool m_bootstrapping = false;
    bool m_bootstrapped = false;
    bool m_started = false;
    bool m_stopped = false;
};
} // namespace lb
