#include "lb/listener.hpp"
#include "lb/client_session.hpp"
#include "lb/proxy_http.hpp"
#include "lb/upstream_proxy.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <csignal>
#include <exception>
#include <iostream>
#include <memory>

namespace lb {
namespace {
namespace asio = boost::asio;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;

asio::awaitable<void> listen(tcp::acceptor& acceptor, std::shared_ptr<ProxyState> state) {
    // One-time fail-closed readiness bootstrap. Recurring policy is Phase 4.
    for (const auto& configured : state->options.backends) {
        const auto backend = state->registry.findBackend(configured.id);
        ProxyRequest probe{http::verb::get, "/health", 11};
        prepareUpstreamRequest(probe, *backend, "127.0.0.1");
        auto result = co_await forwardToBackend(std::move(probe), backend, state->options);
        if (result.error == UpstreamError::NONE && result.response.result() == http::status::ok) {
            state->registry.setState(backend->id, BackendState::HEALTHY);
        }
    }
    std::cout << "{\"event\":\"listening\",\"host\":\"" << state->options.listen
              << "\",\"port\":" << acceptor.local_endpoint().port()
              << ",\"eligible\":" << state->registry.snapshot()->size() << "}" << std::endl;
    for (;;) {
        auto socket = co_await acceptor.async_accept(asio::use_awaitable);
        socket.set_option(tcp::no_delay(true));
        asio::co_spawn(acceptor.get_executor(), runClientSession(std::move(socket), state), asio::detached);
    }
}
} // namespace

void runProxy(const ProxyOptions& options) {
    asio::io_context context(1);
    auto state = std::make_shared<ProxyState>(options);
    tcp::acceptor acceptor(context);
    const tcp::endpoint endpoint(asio::ip::make_address(options.listen), options.port);
    acceptor.open(endpoint.protocol());
    acceptor.set_option(asio::socket_base::reuse_address(true));
    acceptor.bind(endpoint);
    acceptor.listen(asio::socket_base::max_listen_connections);
    asio::signal_set signals(context, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code& error, int) {
        if (!error) {
            context.stop();
        }
    });
    std::exception_ptr failure;
    asio::co_spawn(context, listen(acceptor, state), [&](std::exception_ptr error) {
        failure = error;
        context.stop();
    });
    context.run();
    if (failure) {
        std::rethrow_exception(failure);
    }
}
} // namespace lb
