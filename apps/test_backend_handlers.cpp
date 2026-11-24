#include "lb/test_backend.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <charconv>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace lb::simulator {
namespace {
namespace http = boost::beast::http;

bool validId(std::string_view value, std::size_t limit) {
    if (value.empty() || value.size() > limit) {
        return false;
    }
    for (const char character : value) {
        if (!((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') || character == '-' || character == '_' ||
              character == '.' || character == ':')) {
            return false;
        }
    }
    return true;
}

unsigned parseNumber(std::string_view value, unsigned limit) {
    unsigned number = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
    if (value.empty() || error != std::errc{} || end != value.data() + value.size() ||
        number > limit) {
        throw std::invalid_argument("Invalid or out-of-range unsigned integer");
    }
    return number;
}

unsigned queryNumber(std::string_view target, std::string_view path, std::string_view key,
                     unsigned fallback, unsigned limit) {
    if (target == path) {
        return fallback;
    }
    const std::string prefix = std::string(path) + "?" + std::string(key) + "=";
    if (!target.starts_with(prefix)) {
        throw std::invalid_argument("Unexpected query parameter");
    }
    return parseNumber(target.substr(prefix.size()), limit);
}
} // namespace

Options parseOptions(std::span<const std::string_view> args) {
    Options options;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const auto argument = args[index];
        if (argument == "--help") {
            options.help = true;
        } else if (argument == "--test-mode") {
            options.testMode = true;
        } else if (argument == "--not-ready") {
            options.ready = false;
        } else if (argument == "--id" || argument == "--listen" || argument == "--port") {
            if (++index == args.size()) {
                throw std::invalid_argument("Missing option value");
            }
            if (argument == "--id") {
                options.id = args[index];
            } else if (argument == "--listen") {
                options.listen = args[index];
            } else {
                options.port = static_cast<std::uint16_t>(parseNumber(args[index], 65535));
            }
        } else {
            throw std::invalid_argument("Unknown option: " + std::string(argument));
        }
    }
    validateOptions(options);
    return options;
}

void validateOptions(const Options& options) {
    if (!validId(options.id, 64)) {
        throw std::invalid_argument("Backend id must be 1..64 ASCII letters/digits or -_.:");
    }
    boost::system::error_code error;
    const auto address = boost::asio::ip::make_address(options.listen, error);
    if (error) {
        throw std::invalid_argument("--listen must be a numeric IP address");
    }
    if (options.testMode && !address.is_loopback()) {
        throw std::invalid_argument("--test-mode requires a loopback listen address");
    }
}

Backend::Backend(Options options) : m_options(std::move(options)), m_ready(m_options.ready) {
    validateOptions(m_options);
}

boost::asio::awaitable<Response> Backend::respond(Request request) {
    Response response{http::status::ok, 11};
    response.keep_alive(request.keep_alive());
    response.set(http::field::server, "lb-test-backend");
    response.set(http::field::content_type, "application/json");
    response.set("X-Backend-ID", m_options.id);
    const std::string generated = m_options.id + "-" + std::to_string(++m_sequence);
    const auto supplied = request["X-Request-ID"];
    const std::string requestId =
        supplied.empty() ? generated : std::string(supplied.data(), supplied.size());
    const auto fail = [&](http::status status, std::string_view reason) {
        response.result(status);
        response.body() = "{\"error\":\"" + std::string(reason) + "\"}";
    };
    const bool validRequestId = validId(requestId, 128);
    response.set("X-Request-ID", validRequestId ? requestId : generated);
    std::clog << "{\"event\":\"received\",\"server_id\":\"" << m_options.id
              << "\",\"request_id\":\"" << response["X-Request-ID"] << "\"}" << std::endl;
    if (!validRequestId) {
        fail(http::status::bad_request, "Invalid X-Request-ID");
        if (request.method() == http::verb::head) {
            response.body().clear();
        }
        response.prepare_payload();
        co_return response;
    }
    const auto rawTarget = request.target();
    const std::string_view target(rawTarget.data(), rawTarget.size());
    const auto path = target.substr(0, target.find('?'));
    const auto identity =
        "{\"server_id\":\"" + m_options.id + "\",\"request_id\":\"" + requestId + "\"}";

    if (request.version() != 11) {
        fail(http::status::http_version_not_supported, "Only HTTP/1.1 is supported");
        response.keep_alive(false);
    } else if (request.method() == http::verb::connect ||
               request.count(http::field::upgrade) != 0) {
        fail(http::status::not_implemented, "Tunneling and upgrades are unsupported");
        response.keep_alive(false);
    } else if (request.body().size() > MAX_BODY_BYTES) {
        fail(http::status::payload_too_large, "Body too large");
        response.keep_alive(false);
    } else if (path == "/admin/ready" && !m_options.testMode) {
        fail(http::status::not_found, "Not found");
    } else if (path == "/admin/ready" && request.method() == http::verb::post) {
        if (target == "/admin/ready?value=true" || target == "/admin/ready?value=false") {
            m_ready = target.ends_with("true");
            response.body() = m_ready ? "{\"ready\":true}" : "{\"ready\":false}";
        } else {
            fail(http::status::bad_request, "Expected value=true or value=false");
        }
    } else if (path == "/echo" && request.method() == http::verb::post && target == path) {
        response.body() = std::move(request.body());
        if (request.count(http::field::content_type) != 0) {
            response.set(http::field::content_type, request[http::field::content_type]);
        } else {
            response.set(http::field::content_type, "application/octet-stream");
        }
        if (request.count("X-Echo-Token") != 0) {
            response.set("X-Echo-Token", request["X-Echo-Token"]);
        }
    } else if ((path == "/health" || path == "/request" || path == "/slow" || path == "/error" ||
                path == "/large") &&
               request.method() == http::verb::get) {
        response.body() = identity;
        try {
            if (path == "/health" && target == path) {
                response.result(m_ready ? http::status::ok : http::status::service_unavailable);
            } else if ((path == "/request" || path == "/error") && target == path) {
                if (path == "/error") {
                    response.result(http::status::internal_server_error);
                }
            } else if (path == "/slow") {
                const auto delay = queryNumber(target, path, "ms", 500, MAX_DELAY_MS);
                const auto executor = co_await boost::asio::this_coro::executor;
                boost::asio::steady_timer timer(executor, std::chrono::milliseconds(delay));
                co_await timer.async_wait(boost::asio::use_awaitable);
            } else if (path == "/large") {
                const auto bytes = queryNumber(target, path, "bytes", 65536, MAX_BODY_BYTES);
                response.body().assign(bytes, 'x');
                response.set(http::field::content_type, "application/octet-stream");
            } else {
                fail(http::status::bad_request, "Unexpected query parameter");
            }
        } catch (const std::invalid_argument&) {
            fail(http::status::bad_request, "Invalid query parameter");
        }
    } else if (path == "/health" || path == "/request" || path == "/slow" || path == "/error" ||
               path == "/large" || path == "/echo" || path == "/admin/ready") {
        fail(http::status::method_not_allowed, "Method not allowed");
        response.set(http::field::allow,
                     path == "/echo" || path == "/admin/ready" ? "POST" : "GET");
    } else {
        fail(http::status::not_found, "Not found");
    }
    if (request.method() == http::verb::head) {
        response.body().clear();
    }
    response.prepare_payload();
    co_return response;
}

} // namespace lb::simulator
