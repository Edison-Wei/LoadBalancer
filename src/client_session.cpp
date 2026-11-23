#include "lb/client_session.hpp"
#include "lb/proxy_http.hpp"
#include "lb/upstream_proxy.hpp"

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>

#include <chrono>
#include <exception>
#include <iostream>
#include <utility>

namespace lb {
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

http::status parserStatus(const boost::system::error_code& error) {
    if (error == http::error::body_limit) {
        return http::status::payload_too_large;
    }
    if (error == http::error::header_limit) {
        return http::status::request_header_fields_too_large;
    }
    if (error == beast::error::timeout) {
        return http::status::request_timeout;
    }
    return http::status::bad_request;
}

struct InFlight {
    explicit InFlight(BackendPtr value) : backend(std::move(value)) {
        backend->inFlight.fetch_add(1, std::memory_order_relaxed);
    }
    ~InFlight() { backend->inFlight.fetch_sub(1, std::memory_order_relaxed); }
    BackendPtr backend;
};
} // namespace

asio::awaitable<void> runClientSession(asio::ip::tcp::socket socket, std::shared_ptr<ProxyState> state) {
    beast::tcp_stream client(std::move(socket));
    beast::flat_buffer buffer(state->options.maxBodyBytes + state->options.maxHeaderBytes);
    try {
        const auto address = client.socket().remote_endpoint().address().to_string();
        for (;;) {
            http::request_parser<http::string_body> parser;
            parser.body_limit(state->options.maxBodyBytes);
            parser.header_limit(state->options.maxHeaderBytes);
            client.expires_after(std::chrono::milliseconds(state->options.clientTimeoutMs));
            boost::system::error_code error;
            co_await http::async_read_header(client, buffer, parser,
                                            asio::redirect_error(asio::use_awaitable, error));
            if (error == http::error::end_of_stream) {
                break;
            }
            auto rejection = error ? std::optional<http::status>(parserStatus(error))
                                   : validateClientHeader(parser.get());
            if (!rejection && !parser.is_done()) {
                co_await http::async_read(client, buffer, parser,
                                         asio::redirect_error(asio::use_awaitable, error));
                if (error) {
                    rejection = parserStatus(error);
                }
            }
            if (rejection) {
                auto response = proxyError(*rejection, false, parser.get().method() == http::verb::head);
                client.expires_after(std::chrono::milliseconds(state->options.clientTimeoutMs));
                co_await http::async_write(client, response, asio::redirect_error(asio::use_awaitable, error));
                break;
            }
            auto request = parser.release();
            const bool keepAlive = request.keep_alive();
            const bool head = request.method() == http::verb::head;
            const auto snapshot = state->registry.snapshot();
            const auto backend = state->scheduler.select(snapshot);
            ProxyResponse response;
            if (!backend) {
                response = proxyError(http::status::service_unavailable, keepAlive, head);
            } else if (!prepareUpstreamRequest(request, *backend, address)) {
                response = proxyError(http::status::bad_request, false, head);
            } else {
                InFlight flight(backend);
                backend->requests.fetch_add(1, std::memory_order_relaxed);
                auto upstream = co_await forwardToBackend(std::move(request), backend, state->options);
                if (upstream.error != UpstreamError::NONE) {
                    backend->failures.fetch_add(1, std::memory_order_relaxed);
                    response = proxyError(upstream.error == UpstreamError::TIMEOUT
                                              ? http::status::gateway_timeout
                                              : http::status::bad_gateway,
                                          keepAlive, head);
                } else {
                    response = std::move(upstream.response);
                    if (!prepareClientResponse(response, keepAlive, head)) {
                        backend->failures.fetch_add(1, std::memory_order_relaxed);
                        response = proxyError(http::status::bad_gateway, false, head);
                    }
                }
                std::clog << "{\"event\":\"upstream_complete\",\"backend\":\"" << backend->id
                          << "\",\"status\":" << response.result_int() << "}" << std::endl;
            }
            client.expires_after(std::chrono::milliseconds(state->options.clientTimeoutMs));
            co_await http::async_write(client, response, asio::redirect_error(asio::use_awaitable, error));
            if (error || !response.keep_alive()) {
                break;
            }
        }
    } catch (const std::exception&) {
        std::clog << "{\"event\":\"client_session_error\"}" << std::endl;
    }
    boost::system::error_code ignored;
    client.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
}
} // namespace lb
