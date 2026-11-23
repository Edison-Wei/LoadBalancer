#pragma once

#include "lb/config.hpp"
#include "lb/registry.hpp"
#include "lb/round_robin.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <memory>
#include <utility>

namespace lb {
/// Phase 3 runs one event-loop worker. Each session owns its streams and parser.
struct ProxyState {
    explicit ProxyState(ProxyOptions value) : options(std::move(value)), registry(options.backends) {}
    const ProxyOptions options;
    BackendRegistry registry;
    RoundRobin scheduler;
};

boost::asio::awaitable<void> runClientSession(boost::asio::ip::tcp::socket socket,
                                            std::shared_ptr<ProxyState> state);
} // namespace lb
