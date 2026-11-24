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
    UpstreamStage stage = UpstreamStage::RESOLVE;
};

struct CancelDeadline {
    std::shared_ptr<Operation> operation;
    std::shared_ptr<UpstreamCancellation> cancellation;
    BackendPtr backend;
    bool connected = false;
    ~CancelDeadline() {
        operation->deadline.cancel();
        // Close before returning the caller's admission lease, even if the
        // canceled timer handler still retains this Operation temporarily.
        operation->stream.close();
        if (connected)
            backend->openConnections.fetch_sub(1, std::memory_order_relaxed);
        if (cancellation) {
            cancellation->setAction({});
        }
    }
};
} // namespace

asio::awaitable<UpstreamResult>
forwardToBackend(ProxyRequest request, BackendPtr backend, const ProxyOptions& options,
                 unsigned responseTimeoutMs, std::shared_ptr<UpstreamCancellation> cancellation) {
    const auto executor = co_await asio::this_coro::executor;
    const auto operation = std::make_shared<Operation>(executor);
    CancelDeadline guard{operation, cancellation, backend};
    const auto cancelled = [&] { return cancellation && cancellation->cancelled(); };
    if (cancelled()) {
        co_return UpstreamResult{ProxyResponse{}, UpstreamError::CANCELLED};
    }
    if (cancellation) {
        cancellation->setAction([weak = std::weak_ptr<Operation>(operation)] {
            if (const auto active = weak.lock()) {
                active->resolver.cancel();
                active->stream.cancel();
            }
        });
    }
    operation->deadline.expires_after(std::chrono::milliseconds(options.upstreamTimeoutMs));
    operation->deadline.async_wait([operation](const boost::system::error_code& error) {
        if (!error) {
            operation->timedOut = true;
            operation->resolver.cancel();
            operation->stream.cancel();
        }
    });
    boost::system::error_code error;
    boost::system::error_code addressError;
    const auto address = asio::ip::make_address(backend->host, addressError);
    tcp::resolver::results_type endpoints;
    if (addressError) {
        // Numeric endpoints avoid the system resolver and its platform-specific
        // getaddrinfo cancellation/teardown latency entirely.
        endpoints = co_await operation->resolver.async_resolve(
            backend->host, std::to_string(backend->port),
            asio::redirect_error(asio::use_awaitable, error));
    }
    if (!error && !operation->timedOut && !cancelled()) {
        operation->stage = UpstreamStage::CONNECT;
        operation->stream.expires_after(std::chrono::milliseconds(options.connectTimeoutMs));
        if (!addressError) {
            co_await operation->stream.async_connect(
                tcp::endpoint(address, backend->port),
                asio::redirect_error(asio::use_awaitable, error));
        } else {
            co_await operation->stream.async_connect(
                endpoints, asio::redirect_error(asio::use_awaitable, error));
        }
    }
    if (!error && operation->stream.socket().is_open() &&
        operation->stage == UpstreamStage::CONNECT) {
        guard.connected = true;
        backend->openConnections.fetch_add(1, std::memory_order_relaxed);
    }
    if (!error && !operation->timedOut && !cancelled()) {
        operation->stage = UpstreamStage::WRITE;
        if (responseTimeoutMs) {
            operation->stream.expires_after(std::chrono::milliseconds(responseTimeoutMs));
        } else {
            operation->stream.expires_after(std::chrono::milliseconds(options.writeTimeoutMs));
        }
        co_await http::async_write(operation->stream, request,
                                   asio::redirect_error(asio::use_awaitable, error));
    }
    beast::flat_buffer buffer(options.maxBodyBytes + options.maxHeaderBytes);
    const bool head = request.method() == http::verb::head;
    if (!error && !operation->timedOut && !cancelled()) {
        operation->stage = UpstreamStage::READ;
        if (!responseTimeoutMs) {
            operation->stream.expires_after(std::chrono::milliseconds(options.readTimeoutMs));
        }
    }
    for (int informational = 0; !error && !operation->timedOut && !cancelled() && informational < 8;
         ++informational) {
        http::response_parser<http::string_body> parser;
        parser.body_limit(options.maxBodyBytes);
        parser.header_limit(options.maxHeaderBytes);
        parser.skip(head);
        co_await http::async_read(operation->stream, buffer, parser,
                                  asio::redirect_error(asio::use_awaitable, error));
        if (!error) {
            auto response = parser.release();
            if (operation->timedOut || cancelled()) {
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
    const auto failure =
        cancelled() ? UpstreamError::CANCELLED
        : operation->timedOut || error == beast::error::timeout || error == asio::error::timed_out
            ? UpstreamError::TIMEOUT
            : UpstreamError::BAD_GATEWAY;
    const auto reason =
        failure == UpstreamError::TIMEOUT ? UpstreamReason::TIMEOUT
        : error == asio::error::connection_reset || error == asio::error::connection_aborted
            ? UpstreamReason::RESET
        : operation->stage == UpstreamStage::RESOLVE ? UpstreamReason::RESOLVE
        : operation->stage == UpstreamStage::CONNECT ? UpstreamReason::CONNECT
        : error.category() == http::make_error_code(http::error::bad_method).category() || !error
            ? UpstreamReason::PROTOCOL
            : UpstreamReason::TRANSPORT;
    co_return UpstreamResult{ProxyResponse{}, failure, operation->stage, operation->timedOut,
                             reason};
}
} // namespace lb
