#include "lb/test_backend.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <memory>
#include <utility>

namespace lb::simulator {
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

void logRequest(std::string_view event, const Response& response) {
    std::clog << "{\"event\":\"" << event << "\",\"server_id\":\"" << response["X-Backend-ID"]
              << "\",\"request_id\":\"" << response["X-Request-ID"] << "\",\"status\":"
              << response.result_int() << "}" << std::endl;
}

asio::awaitable<void> session(tcp::socket socket, std::shared_ptr<Backend> backend) {
    beast::tcp_stream stream(std::move(socket));
    beast::flat_buffer buffer(MAX_BODY_BYTES + MAX_HEADER_BYTES);
    try {
        for (;;) {
            http::request_parser<http::string_body> parser;
            parser.body_limit(MAX_BODY_BYTES);
            parser.header_limit(MAX_HEADER_BYTES);
            stream.expires_after(std::chrono::seconds(15));
            boost::system::error_code error;
            co_await http::async_read(stream, buffer, parser,
                                     asio::redirect_error(asio::use_awaitable, error));
            if (error == http::error::end_of_stream) {
                break;
            }
            if (error) {
                Response response{error == http::error::body_limit ? http::status::payload_too_large
                                  : error == http::error::header_limit
                                      ? http::status::request_header_fields_too_large
                                      : http::status::bad_request,
                                  11};
                response.body() = "Rejected HTTP request\n";
                response.keep_alive(false);
                response.prepare_payload();
                co_await http::async_write(stream, response,
                                          asio::redirect_error(asio::use_awaitable, error));
                break;
            }
            auto response = co_await backend->respond(parser.release());
            stream.expires_after(std::chrono::seconds(15));
            co_await http::async_write(stream, response,
                                      asio::redirect_error(asio::use_awaitable, error));
            logRequest(error ? "request_failed" : "completed", response);
            if (error || !response.keep_alive()) {
                break;
            }
        }
    } catch (const std::exception&) {
        std::clog << "{\"event\":\"session_error\"}\n";
    }
    boost::system::error_code ignored;
    stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
}

asio::awaitable<void> acceptConnections(tcp::acceptor& acceptor, std::shared_ptr<Backend> backend) {
    for (;;) {
        auto socket = co_await acceptor.async_accept(asio::use_awaitable);
        asio::co_spawn(acceptor.get_executor(), session(std::move(socket), backend), asio::detached);
    }
}
} // namespace

void run(const Options& options) {
    validateOptions(options);
    asio::io_context context(1);
    auto backend = std::make_shared<Backend>(options);
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
    asio::co_spawn(context, acceptConnections(acceptor, backend), [&](std::exception_ptr error) {
        failure = error;
        context.stop();
    });
    std::cout << "{\"event\":\"listening\",\"server_id\":\"" << options.id << "\",\"host\":\""
              << options.listen << "\",\"port\":" << acceptor.local_endpoint().port() << "}" << std::endl;
    context.run();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

} // namespace lb::simulator
