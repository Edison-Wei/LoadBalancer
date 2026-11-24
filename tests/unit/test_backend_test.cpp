#include "lb/test_backend.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <exception>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace simulator = lb::simulator;
namespace asio = boost::asio;
namespace http = boost::beast::http;

simulator::Request request(http::verb method, std::string_view target) {
    simulator::Request result{method, std::string(target), 11};
    result.set(http::field::host, "localhost");
    return result;
}

simulator::Response respond(simulator::Backend& backend, simulator::Request incoming) {
    asio::io_context context;
    auto future = asio::co_spawn(context, backend.respond(std::move(incoming)), asio::use_future);
    context.run();
    return future.get();
}

TEST(TestBackend, ParsesOptionsAndRejectsInvalidOrExposedTestMode) {
    const std::vector<std::string_view> args = {
        "--id", "backend-1", "--listen", "127.0.0.1", "--port", "0", "--test-mode", "--not-ready"};
    const auto options = simulator::parseOptions(args);
    EXPECT_EQ(options.id, "backend-1");
    EXPECT_EQ(options.port, 0);
    EXPECT_TRUE(options.testMode);
    EXPECT_FALSE(options.ready);
    for (const auto& invalid :
         std::vector<std::vector<std::string_view>>{{"--port", "65536"},
                                                    {"--port", "-1"},
                                                    {"--port", "80junk"},
                                                    {"--port"},
                                                    {"--unknown"},
                                                    {"--id", "bad\"id"},
                                                    {"--listen", "localhost"},
                                                    {"--test-mode", "--listen", "0.0.0.0"},
                                                    {"--test-mode", "--listen", "::"}}) {
        EXPECT_THROW(simulator::parseOptions(invalid), std::invalid_argument);
    }
    const std::vector<std::string_view> ipv6 = {"--test-mode", "--listen", "::1"};
    EXPECT_NO_THROW(simulator::parseOptions(ipv6));
}

TEST(TestBackend, ReturnsStableBackendAndUniqueOrSuppliedRequestIds) {
    simulator::Options options;
    options.id = "backend-A";
    simulator::Backend backend(options);
    const auto first = respond(backend, request(http::verb::get, "/request"));
    const auto second = respond(backend, request(http::verb::get, "/request"));
    EXPECT_EQ(first.result_int(), 200);
    EXPECT_EQ(first["X-Backend-ID"], "backend-A");
    EXPECT_EQ(first["X-Request-ID"], "backend-A-1");
    EXPECT_NE(first["X-Request-ID"], second["X-Request-ID"]);
    EXPECT_NE(first.body().find("\"server_id\":\"backend-A\""), std::string::npos);
    auto correlated = request(http::verb::get, "/request");
    correlated.set("X-Request-ID", "client-42");
    const auto response = respond(backend, std::move(correlated));
    EXPECT_EQ(response["X-Request-ID"], "client-42");
    EXPECT_NE(response.body().find("\"request_id\":\"client-42\""), std::string::npos);
    auto invalid = request(http::verb::get, "/request");
    invalid.set("X-Request-ID", "bad\"id");
    EXPECT_EQ(respond(backend, std::move(invalid)).result_int(), 400);
}

TEST(TestBackend, ReadinessToggleIsOptInAndHealthDoesNotControlBusinessResponses) {
    simulator::Options options;
    options.testMode = true;
    simulator::Backend backend(options);
    EXPECT_EQ(respond(backend, request(http::verb::get, "/health")).result_int(), 200);
    EXPECT_EQ(respond(backend, request(http::verb::post, "/admin/ready?value=false")).result_int(),
              200);
    EXPECT_EQ(respond(backend, request(http::verb::get, "/health")).result_int(), 503);
    EXPECT_EQ(respond(backend, request(http::verb::get, "/request")).result_int(), 200);
    EXPECT_EQ(respond(backend, request(http::verb::post, "/admin/ready?value=true")).result_int(),
              200);
    EXPECT_EQ(respond(backend, request(http::verb::get, "/health")).result_int(), 200);
    EXPECT_EQ(respond(backend, request(http::verb::post, "/admin/ready?value=wrong")).result_int(),
              400);
    simulator::Backend normal(simulator::Options{});
    EXPECT_EQ(respond(normal, request(http::verb::post, "/admin/ready?value=false")).result_int(),
              404);
    options.ready = false;
    simulator::Backend initiallyUnready(options);
    EXPECT_EQ(respond(initiallyUnready, request(http::verb::get, "/health")).result_int(), 503);
}

TEST(TestBackend, EchoPreservesBinaryBodyAndSelectedHeaders) {
    simulator::Backend backend(simulator::Options{});
    auto incoming = request(http::verb::post, "/echo");
    const std::string body("hello\0world", 11);
    incoming.body() = body;
    incoming.set(http::field::content_type, "application/octet-stream");
    incoming.set("X-Echo-Token", "echo-42");
    incoming.set("X-Request-ID", "echo-request");
    incoming.prepare_payload();
    const auto response = respond(backend, std::move(incoming));
    EXPECT_EQ(response.body(), body);
    EXPECT_EQ(response[http::field::content_type], "application/octet-stream");
    EXPECT_EQ(response["X-Echo-Token"], "echo-42");
    EXPECT_EQ(response["X-Request-ID"], "echo-request");
    auto oversized = request(http::verb::post, "/echo");
    oversized.body().assign(simulator::MAX_BODY_BYTES + 1, 'x');
    const auto rejected = respond(backend, std::move(oversized));
    EXPECT_EQ(rejected.result_int(), 413);
    EXPECT_FALSE(rejected.keep_alive());
}

TEST(TestBackend, ErrorAndBoundedLargePayloadAreDeterministic) {
    simulator::Backend backend(simulator::Options{});
    EXPECT_EQ(respond(backend, request(http::verb::get, "/error")).result_int(), 500);
    EXPECT_EQ(respond(backend, request(http::verb::get, "/large")).body().size(), 65536U);
    EXPECT_EQ(respond(backend, request(http::verb::get, "/large?bytes=0")).body(), "");
    EXPECT_EQ(respond(backend, request(http::verb::get, "/large?bytes=1024")).body(),
              std::string(1024, 'x'));
    EXPECT_EQ(respond(backend, request(http::verb::get, "/large?bytes=1048576")).body().size(),
              simulator::MAX_BODY_BYTES);
    for (const auto* target : {"/large?bytes=1048577", "/large?bytes=-1", "/large?wrong=2",
                               "/slow?ms=5001", "/slow?ms=-1", "/slow?ms=2&ms=3", "/slow?ms="}) {
        EXPECT_EQ(respond(backend, request(http::verb::get, target)).result_int(), 400) << target;
    }
}

TEST(TestBackend, AsyncDelayAllowsAnotherRequestToFinishFirst) {
    simulator::Backend backend(simulator::Options{});
    asio::io_context context;
    std::vector<std::string> completed;
    std::exception_ptr failure;
    const auto started = std::chrono::steady_clock::now();
    asio::co_spawn(context, backend.respond(request(http::verb::get, "/slow?ms=25")),
                   [&](std::exception_ptr error, simulator::Response response) {
                       if (error) {
                           failure = error;
                       }
                       EXPECT_EQ(response.result_int(), 200);
                       completed.emplace_back("slow");
                   });
    asio::co_spawn(context, backend.respond(request(http::verb::get, "/request")),
                   [&](std::exception_ptr error, simulator::Response response) {
                       if (error) {
                           failure = error;
                       }
                       EXPECT_EQ(response.result_int(), 200);
                       completed.emplace_back("fast");
                   });
    context.run();
    EXPECT_FALSE(failure);
    EXPECT_EQ(completed, (std::vector<std::string>{"fast", "slow"}));
    EXPECT_GE(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(25));
}

TEST(TestBackend, RejectsUnknownRoutesWrongMethodsAndUnsupportedProtocols) {
    simulator::Backend backend(simulator::Options{});
    EXPECT_EQ(respond(backend, request(http::verb::get, "/missing")).result_int(), 404);
    const auto wrongMethod = respond(backend, request(http::verb::post, "/health"));
    EXPECT_EQ(wrongMethod.result_int(), 405);
    EXPECT_EQ(wrongMethod[http::field::allow], "GET");
    const auto head = respond(backend, request(http::verb::head, "/health"));
    EXPECT_EQ(head.result_int(), 405);
    EXPECT_TRUE(head.body().empty());
    auto oldHttp = request(http::verb::get, "/request");
    oldHttp.version(10);
    EXPECT_EQ(respond(backend, std::move(oldHttp)).result_int(), 505);
    const auto connect = respond(backend, request(http::verb::connect, "localhost:80"));
    EXPECT_EQ(connect.result_int(), 501);
    EXPECT_FALSE(connect.keep_alive());
    auto upgrade = request(http::verb::get, "/request");
    upgrade.set(http::field::upgrade, "websocket");
    EXPECT_EQ(respond(backend, std::move(upgrade)).result_int(), 501);
    auto closing = request(http::verb::get, "/request");
    closing.keep_alive(false);
    EXPECT_FALSE(respond(backend, std::move(closing)).keep_alive());
}

} // namespace
