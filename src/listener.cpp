#include "lb/listener.hpp"
#include "lb/client_session.hpp"
#include "lb/health_checker.hpp"
#include "lb/log.hpp"
#include "lb/metrics_server.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <csignal>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lb {
namespace {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

/// Acceptor, health checker, session membership, and shutdown are all serialized
/// by m_control. Each accepted socket uses a distinct strand on the same context.
class Runtime : public std::enable_shared_from_this<Runtime> {
public:
    Runtime(asio::io_context& context, const ProxyOptions& options,
            std::function<void(std::exception_ptr)> recordFailure)
        : m_context(context), m_control(asio::make_strand(context)), m_acceptor(m_control),
          m_signals(m_control, SIGINT, SIGTERM), m_deadline(m_control),
          m_guard(asio::make_work_guard(context)), m_state(std::make_shared<ProxyState>(options)),
          m_recordFailure(std::move(recordFailure)) {}

    void start() {
        const tcp::endpoint endpoint(asio::ip::make_address(m_state->options.listen), m_state->options.port);
        m_acceptor.open(endpoint.protocol());
        m_acceptor.set_option(asio::socket_base::reuse_address(true));
        m_acceptor.bind(endpoint);
        m_acceptor.listen(asio::socket_base::max_listen_connections);
        m_health = std::make_shared<HealthChecker>(m_control, m_state->registry, m_state->options.health,
            HealthChecker::Probe{}, [weak = weak_from_this()](std::exception_ptr error) {
                if (const auto self = weak.lock()) self->fail(error);
            });
        m_metrics = std::make_shared<MetricsServer>(m_control, m_state);
        m_metrics->start([weak = weak_from_this()](std::exception_ptr error) {
            if (const auto self = weak.lock()) self->fail(error);
        });
        waitSignal();
        asio::co_spawn(m_control, acceptConnections(), [self = shared_from_this()](std::exception_ptr error) {
            if (error) self->fail(error);
        });
    }

    auto executor() const { return m_control; }

    void report() const {
        LogLine{} << "{\"event\":\"" << (m_finished ? "shutdown_complete" : "runtime_stopped")
                  << "\",\"forced\":" << (m_forced ? "true" : "false")
                  << ",\"workers_joined\":" << m_state->options.workers
                  << ",\"connections\":" << m_state->connections.used()
                  << ",\"requests\":" << m_state->requests.used()
                  << ",\"upstreams\":" << m_state->upstreams.used()
                  << ",\"peak_connections\":" << m_state->connections.peak()
                  << ",\"peak_requests\":" << m_state->requests.peak()
                  << ",\"peak_upstreams\":" << m_state->upstreams.peak()
                  << ",\"rejected_connections\":" << m_state->rejectedConnections.load()
                  << ",\"rejected_requests\":" << m_state->rejectedRequests.load()
                  << ",\"reset_cancellations\":" << m_state->resetCancellations.load()
                  << ",\"queued_requests\":0}";
    }

private:
    asio::awaitable<void> acceptConnections() {
        co_await m_health->bootstrap();
        if (m_state->stopping.load(std::memory_order_acquire)) co_return;
        m_health->start();
        std::cout << "{\"event\":\"listening\",\"host\":\"" << m_state->options.listen
                  << "\",\"port\":" << m_acceptor.local_endpoint().port()
                  << ",\"eligible\":" << m_state->registry->snapshot()->size()
                  << ",\"metrics_port\":" << m_metrics->port()
                  << ",\"workers\":" << m_state->options.workers << "}" << std::endl;
        while (!m_state->stopping.load(std::memory_order_acquire)) {
            boost::system::error_code error;
            auto socket = co_await m_acceptor.async_accept(asio::make_strand(m_context),
                asio::redirect_error(asio::use_awaitable, error));
            if (m_state->stopping.load(std::memory_order_acquire)) co_return;
            if (error) throw boost::system::system_error(error);
            auto lease = m_state->connections.tryAcquire();
            if (!lease) {
                m_state->rejectedConnections.fetch_add(1, std::memory_order_relaxed);
                socket.close(error); // no unbounded coroutine/response queue for excess sockets
                LogLine{} << "{\"event\":\"connection_rejected\"}";
                continue;
            }
            socket.set_option(tcp::no_delay(true));
            const auto executor = socket.get_executor();
            const auto id = ++m_nextId;
            auto session = std::make_shared<ClientSession>(std::move(socket), m_state, std::move(*lease), id);
            m_sessions.emplace(id, session);
            LogLine{} << "{\"event\":\"connection_accepted\",\"connection\":" << id << "}";
            asio::co_spawn(executor, session->run(), asio::bind_executor(m_control,
                [self = shared_from_this(), session, id](std::exception_ptr sessionError) {
                    if (sessionError) LogLine{} << "{\"event\":\"client_session_error\"}";
                    self->m_sessions.erase(id);
                    LogLine{} << "{\"event\":\"connection_closed\",\"connection\":" << id << "}";
                    if (self->m_state->stopping.load(std::memory_order_acquire) && self->m_sessions.empty())
                        self->finish();
                }));
        }
    }

    void waitSignal() {
        m_signals.async_wait([self = shared_from_this()](const boost::system::error_code& error, int) {
            if (error) return;
            if (self->m_state->stopping.load(std::memory_order_acquire)) self->force();
            else self->shutdown();
            if (!self->m_finished) self->waitSignal();
        });
    }

    void fail(std::exception_ptr error) {
        m_recordFailure(error);
        shutdown();
        force();
    }

    void shutdown() {
        if (m_state->stopping.exchange(true, std::memory_order_acq_rel)) return;
        boost::system::error_code ignored;
        m_acceptor.cancel(ignored);
        m_acceptor.close(ignored);
        m_health->stop();
        m_metrics->stop();
        LogLine{} << "{\"event\":\"shutdown_started\",\"connections\":" << m_sessions.size() << "}";
        for (const auto& [id, session] : m_sessions) {
            (void)id;
            session->requestDrain();
        }
        if (m_sessions.empty()) {
            finish();
            return;
        }
        m_deadline.expires_after(std::chrono::milliseconds(m_state->options.shutdownGraceMs));
        m_deadline.async_wait([self = shared_from_this()](const boost::system::error_code& error) {
            if (!error) self->force();
        });
    }

    void force() {
        if (m_forced || m_finished) return;
        m_forced = true;
        LogLine{} << "{\"event\":\"shutdown_forced\",\"connections\":" << m_sessions.size() << "}";
        for (const auto& [id, session] : m_sessions) {
            (void)id;
            session->forceClose();
        }
        if (m_sessions.empty()) finish();
    }

    void finish() {
        if (m_finished) return;
        m_finished = true;
        m_deadline.cancel();
        m_signals.cancel();
        m_guard.reset(); // canceled I/O handlers still drain; no normal context.stop()
    }

    asio::io_context& m_context;
    asio::strand<asio::io_context::executor_type> m_control;
    tcp::acceptor m_acceptor;
    asio::signal_set m_signals;
    asio::steady_timer m_deadline;
    asio::executor_work_guard<asio::io_context::executor_type> m_guard;
    std::shared_ptr<ProxyState> m_state;
    std::shared_ptr<HealthChecker> m_health;
    std::shared_ptr<MetricsServer> m_metrics;
    std::function<void(std::exception_ptr)> m_recordFailure;
    std::unordered_map<std::uint64_t, std::shared_ptr<ClientSession>> m_sessions;
    std::uint64_t m_nextId = 0;
    bool m_forced = false;
    bool m_finished = false;
};
} // namespace

void runProxy(const ProxyOptions& options) {
    if (!options.workers || options.workers > 64) throw std::invalid_argument("workers must be 1..64");
    asio::io_context context;
    std::mutex failureMutex;
    std::exception_ptr failure;
    const auto recordFailure = [&](std::exception_ptr error) {
        const std::lock_guard lock(failureMutex);
        if (!failure) failure = error;
    };
    auto runtime = std::make_shared<Runtime>(context, options, recordFailure);
    asio::dispatch(runtime->executor(), [runtime] { runtime->start(); });
    const auto run = [&] {
        try {
            context.run();
        } catch (...) {
            recordFailure(std::current_exception());
            context.stop(); // unexpected worker failure, not the graceful path
        }
    };
    std::vector<std::jthread> workers;
    try {
        for (unsigned index = 0; index < options.workers; ++index) workers.emplace_back(run);
    } catch (...) {
        context.stop();
        workers.clear();
        throw;
    }
    for (auto& worker : workers) worker.join();
    runtime->report();
    if (failure) std::rethrow_exception(failure);
}
} // namespace lb
