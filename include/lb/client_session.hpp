#pragma once

#include "lb/config.hpp"
#include "lb/metrics.hpp"
#include "lb/registry.hpp"
#include "lb/resource_limiter.hpp"
#include "lb/round_robin.hpp"
#include "lb/upstream_proxy.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/tcp_stream.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

namespace lb {
struct ProxyState {
    explicit ProxyState(ProxyOptions value)
        : options(std::move(value)), registry(std::make_shared<BackendRegistry>(options.backends)),
          connections(options.maxConnections), requests(options.maxInFlight),
          upstreams(options.maxUpstreamConnections) {}
    const ProxyOptions options;
    std::shared_ptr<BackendRegistry> registry;
    RoundRobin scheduler;
    MetricsRegistry metrics;
    std::atomic<std::uint64_t> openDownstream{0};
    ResourceLimiter connections;
    ResourceLimiter requests;
    ResourceLimiter upstreams;
    std::atomic<bool> stopping{false};
    std::atomic<std::uint64_t> rejectedConnections{0};
    std::atomic<std::uint64_t> rejectedRequests{0};
    std::atomic<std::uint64_t> requestLimitRejections{0};
    std::atomic<std::uint64_t> upstreamLimitRejections{0};
    std::atomic<std::uint64_t> shutdownRejections{0};
    std::atomic<std::uint64_t> resetCancellations{0};
};

/// Socket, timers, cancellation handles, and flags are confined to this socket's
/// strand. Public shutdown methods dispatch onto that executor.
class ClientSession : public std::enable_shared_from_this<ClientSession> {
public:
    ClientSession(boost::asio::ip::tcp::socket socket, std::shared_ptr<ProxyState> state,
                  ResourceLimiter::Lease connection, std::uint64_t id);
    boost::asio::awaitable<void> run();
    void requestDrain();
    void forceClose();
private:
    boost::asio::awaitable<bool> writeResponse(ProxyResponse& response);
    void watchDisconnect();
    void stopWatching();
    void closeNow();
    std::shared_ptr<ProxyState> m_state;
    ResourceLimiter::Lease m_connection;
    boost::beast::tcp_stream m_client;
    const std::uint64_t m_id;
    bool m_requestActive = false;
    bool m_draining = false;
    bool m_closed = false;
    bool m_reset = false;
    std::shared_ptr<UpstreamCancellation> m_active;
    std::shared_ptr<boost::asio::cancellation_signal> m_watchSignal;
    std::uint64_t m_watchGeneration = 0;
    std::string m_method = "OTHER";
    std::chrono::steady_clock::time_point m_requestStarted;
};
} // namespace lb
