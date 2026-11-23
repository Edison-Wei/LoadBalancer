#include "lb/foundation.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/system_error.hpp>

#include <chrono>
#include <exception>
#include <stdexcept>
#include <string_view>

namespace lb {
namespace {

boost::asio::awaitable<void> waitForTimer(bool& completed) {
    const auto executor = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer timer(executor, std::chrono::milliseconds(1));
    co_await timer.async_wait(boost::asio::use_awaitable);
    completed = true;
}

} // namespace

FoundationResult runFoundationSmoke() {
    FoundationResult result;
    boost::asio::io_context context;
    std::exception_ptr timerError;
    boost::asio::co_spawn(context, waitForTimer(result.timerCompleted),
                          [&timerError](std::exception_ptr error) { timerError = error; });
    context.run();
    if (timerError) {
        std::rethrow_exception(timerError);
    }

    constexpr std::string_view REQUEST =
        "POST /phase-0 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello";
    boost::beast::http::request_parser<boost::beast::http::string_body> parser;
    parser.body_limit(1024);
    parser.header_limit(4096);
    parser.eager(true);
    boost::system::error_code error;
    const auto consumed = parser.put(boost::asio::buffer(REQUEST.data(), REQUEST.size()), error);
    if (error) {
        throw boost::system::system_error(error);
    }
    if (!parser.is_done() || consumed != REQUEST.size()) {
        throw std::runtime_error("Foundation HTTP request was not parsed completely");
    }

    const auto& request = parser.get();
    const auto method = request.method_string();
    const auto target = request.target();
    const auto host = request[boost::beast::http::field::host];
    result.method.assign(method.data(), method.size());
    result.target.assign(target.data(), target.size());
    result.host.assign(host.data(), host.size());
    result.body = request.body();
    return result;
}

} // namespace lb
