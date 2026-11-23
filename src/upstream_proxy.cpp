#include "lb/upstream_proxy.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>

#include <chrono>
#include <memory>
#include <utility>

namespace lb {
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

struct Operation {
    explicit Operation(asio::any_io_executor executor)
        : resolver(executor), stream(executor), deadline(executor) {}
    tcp::resolver resolver;
    beast::tcp_stream stream;
    asio::steady_timer deadline;
    bool timedOut = false;
};

struct CancelDeadline {
    std::shared_ptr<Operation> operation;
    ~CancelDeadline() {
        operation->deadline.cancel();
    }
};
} // namespace

asio::awaitable<UpstreamResult> forwardToBackend(ProxyRequest request, BackendPtr backend,
                                              const ProxyOptions& options) {
    const auto executor = co_await asio::this_coro::executor;
    const auto operation = std::make_shared<Operation>(executor);
    CancelDeadline guard{operation};
    operation->deadline.expires_after(std::chrono::milliseconds(options.upstreamTimeoutMs));
    operation->deadline.async_wait([operation](const boost::system::error_code& error) {
        if (!error) {
            operation->timedOut = true;
            operation->resolver.cancel();
            operation->stream.cancel();
        }
    });
    boost::system::error_code error;
    const auto endpoints = co_await operation->resolver.async_resolve(
        backend->host, std::to_string(backend->port), asio::redirect_error(asio::use_awaitable, error));
    if (!error && !operation->timedOut) {
        operation->stream.expires_after(std::chrono::milliseconds(options.connectTimeoutMs));
        co_await operation->stream.async_connect(endpoints, asio::redirect_error(asio::use_awaitable, error));
    }
    if (!error && !operation->timedOut) {
        operation->stream.expires_never(); // overall deadline still covers all I/O
        co_await http::async_write(operation->stream, request, asio::redirect_error(asio::use_awaitable, error));
    }
    beast::flat_buffer buffer(options.maxBodyBytes + options.maxHeaderBytes);
    const bool head = request.method() == http::verb::head;
    for (int informational = 0; !error && !operation->timedOut && informational < 8; ++informational) {
        http::response_parser<http::string_body> parser;
        parser.body_limit(options.maxBodyBytes);
        parser.header_limit(options.maxHeaderBytes);
        parser.skip(head);
        co_await http::async_read(operation->stream, buffer, parser,
                                 asio::redirect_error(asio::use_awaitable, error));
        if (!error) {
            auto response = parser.release();
            if (operation->timedOut) {
                break;
            }
            if (response.result_int() >= 200) {
                co_return UpstreamResult{std::move(response), UpstreamError::NONE};
            }
            if (response.result() == http::status::switching_protocols) {
                break;
            }
        }
    }
    const auto failure = operation->timedOut || error == beast::error::timeout ||
                                 error == asio::error::timed_out
                             ? UpstreamError::TIMEOUT
                             : UpstreamError::BAD_GATEWAY;
    co_return UpstreamResult{ProxyResponse{}, failure};
}
} // namespace lb
