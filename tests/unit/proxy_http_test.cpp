#include "lb/config.hpp"
#include "lb/proxy_http.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace http = boost::beast::http;

lb::ProxyRequest get() {
    lb::ProxyRequest request{http::verb::get, "/request", 11};
    request.set(http::field::host, "public.example");
    return request;
}

TEST(ProxyConfig, ParsesEndpointsLimitsAndRejectsInvalidInput) {
    const std::vector<std::string_view> args = {"--port",
                                                "0",
                                                "--backend",
                                                "A,127.0.0.1,9001",
                                                "--backend",
                                                "B,::1,9002",
                                                "--max-body-bytes",
                                                "512",
                                                "--upstream-timeout-ms",
                                                "200"};
    const auto options = lb::parseProxyOptions(args);
    ASSERT_EQ(options.backends.size(), 2U);
    EXPECT_EQ(options.port, 0);
    EXPECT_EQ(options.backends[1].host, "::1");
    EXPECT_EQ(options.maxBodyBytes, 512U);
    EXPECT_EQ(options.upstreamTimeoutMs, 200U);
    EXPECT_TRUE(lb::parseProxyOptions({}).backends.empty());
    for (const auto& invalid : std::vector<std::vector<std::string_view>>{
             {"--port", "65536"},
             {"--backend", "A,host,0"},
             {"--backend", "A:host:80"},
             {"--backend", "A,host,80", "--backend", "A,other,90"},
             {"--backend", "A,bad host,80"},
             {"--listen", "localhost"},
             {"--max-body-bytes", "16777217"},
             {"--connect-timeout-ms", "0"},
             {"--port"},
             {"--unknown", "1"}}) {
        EXPECT_THROW(lb::parseProxyOptions(invalid), std::invalid_argument);
    }
}

TEST(ProxyHttp, ValidatesVersionHostTargetMethodsAndFraming) {
    EXPECT_FALSE(lb::validateClientHeader(get()));
    auto request = get();
    request.version(10);
    EXPECT_EQ(lb::validateClientHeader(request), http::status::http_version_not_supported);
    request = get();
    request.erase(http::field::host);
    EXPECT_EQ(lb::validateClientHeader(request), http::status::bad_request);
    request = get();
    request.insert(http::field::host, "second.example");
    EXPECT_EQ(lb::validateClientHeader(request), http::status::bad_request);
    request = get();
    request.target("http://unconfigured.example/");
    EXPECT_EQ(lb::validateClientHeader(request), http::status::bad_request);
    request = get();
    request.set(http::field::content_length, "1");
    request.set(http::field::transfer_encoding, "chunked");
    EXPECT_EQ(lb::validateClientHeader(request), http::status::bad_request);
    request = get();
    request.set(http::field::expect, "100-continue");
    EXPECT_EQ(lb::validateClientHeader(request), http::status::expectation_failed);
    request = get();
    request.method(http::verb::connect);
    EXPECT_EQ(lb::validateClientHeader(request), http::status::not_implemented);
    request = get();
    request.set(http::field::upgrade, "websocket");
    EXPECT_EQ(lb::validateClientHeader(request), http::status::not_implemented);
    request = get();
    request.set(http::field::transfer_encoding, "gzip, chunked");
    EXPECT_EQ(lb::validateClientHeader(request), http::status::not_implemented);
    request = get();
    request.method(http::verb::trace);
    EXPECT_EQ(lb::validateClientHeader(request), http::status::method_not_allowed);
}

TEST(ProxyHttp, RemovesEveryConnectionTokenAndDeclaredTrailersCaseInsensitively) {
    auto request = get();
    request.set(http::field::connection, "keep-alive, X-Secret");
    request.insert(http::field::connection, "\tX-Other\t");
    request.set("x-secret", "hidden");
    request.set("X-Other", "hidden");
    request.set(http::field::trailer, "X-End");
    request.set("X-End", "hidden");
    request.set(http::field::te, "trailers");
    request.set(http::field::proxy_authorization, "secret");
    request.set("X-End-To-End", "retained");
    ASSERT_TRUE(lb::stripHopByHop(request.base()));
    for (const auto* name :
         {"Connection", "X-Secret", "X-Other", "Trailer", "X-End", "TE", "Proxy-Authorization"}) {
        EXPECT_EQ(request.count(name), 0U) << name;
    }
    EXPECT_EQ(request["X-End-To-End"], "retained");
    request.set(http::field::connection, "valid, bad token");
    EXPECT_FALSE(lb::stripHopByHop(request.base()));
}

TEST(ProxyHttp, RewritesHostForwardingIdentityViaAndBodyFraming) {
    auto request = get();
    request.method(http::verb::post);
    request.body() = std::string("a\0b", 3);
    request.chunked(true);
    request.set("Forwarded", "for=spoof");
    request.set("X-Forwarded-For", "spoof");
    request.set("X-Forwarded-Host", "spoof");
    request.set("X-Forwarded-Proto", "https");
    request.set("X-Forwarded-Port", "443");
    request.set("X-Real-IP", "spoof");
    request.set(http::field::via, "1.0 earlier");
    request.set(http::field::content_type, "application/octet-stream");
    const lb::BackendServer backend("B", "::1", 9001);
    ASSERT_TRUE(lb::prepareUpstreamRequest(request, backend, "127.0.0.1"));
    EXPECT_EQ(request[http::field::host], "[::1]:9001");
    EXPECT_EQ(request["X-Forwarded-For"], "127.0.0.1");
    EXPECT_EQ(request["X-Forwarded-Proto"], "http");
    EXPECT_EQ(request.count("Forwarded"), 0U);
    EXPECT_EQ(request.count("X-Forwarded-Host"), 0U);
    EXPECT_EQ(request.count("X-Forwarded-Port"), 0U);
    EXPECT_EQ(request.count("X-Real-IP"), 0U);
    EXPECT_EQ(request[http::field::via], "1.0 earlier, 1.1 load-balancer");
    EXPECT_EQ(request[http::field::content_length], "3");
    EXPECT_FALSE(request.chunked());
    EXPECT_FALSE(request.keep_alive());
    EXPECT_EQ(request.body(), std::string("a\0b", 3));
}

TEST(ProxyHttp, PreservesStatusBodyRepeatedHeadersAndClientKeepAlive) {
    lb::ProxyResponse response{http::status::internal_server_error, 11};
    response.body() = "business error";
    response.set(http::field::connection, "close, X-Internal");
    response.set("X-Internal", "hidden");
    response.set(http::field::transfer_encoding, "chunked");
    response.set(http::field::via, "1.0 origin");
    response.insert(http::field::set_cookie, "one=1");
    response.insert(http::field::set_cookie, "two=2");
    ASSERT_TRUE(lb::prepareClientResponse(response, true, false));
    EXPECT_EQ(response.result_int(), 500);
    EXPECT_EQ(response.body(), "business error");
    EXPECT_EQ(response.count("X-Internal"), 0U);
    EXPECT_EQ(response.count(http::field::set_cookie), 2U);
    EXPECT_EQ(response[http::field::via], "1.0 origin, 1.1 load-balancer");
    EXPECT_TRUE(response.keep_alive());
    EXPECT_FALSE(response.chunked());
    EXPECT_EQ(response[http::field::content_length], "14");
    response.set(http::field::transfer_encoding, "gzip, chunked");
    EXPECT_FALSE(lb::prepareClientResponse(response, true, false));
}

TEST(ProxyHttp, PreservesHeadAndNotModifiedLengthsAndHandlesNoContent) {
    lb::ProxyResponse head{http::status::ok, 11};
    head.content_length(12345);
    ASSERT_TRUE(lb::prepareClientResponse(head, true, true));
    EXPECT_EQ(head[http::field::content_length], "12345");
    EXPECT_TRUE(head.body().empty());
    lb::ProxyResponse unchanged{http::status::not_modified, 11};
    unchanged.content_length(456);
    ASSERT_TRUE(lb::prepareClientResponse(unchanged, true, false));
    EXPECT_EQ(unchanged[http::field::content_length], "456");
    lb::ProxyResponse empty{http::status::no_content, 11};
    ASSERT_TRUE(lb::prepareClientResponse(empty, true, false));
    EXPECT_TRUE(empty.body().empty());
    EXPECT_FALSE(empty.chunked());
}

TEST(ProxyHttp, LocalErrorsHaveDeterministicStatusAndHeadHasNoBody) {
    const auto unavailable = lb::proxyError(http::status::service_unavailable, true);
    EXPECT_EQ(unavailable.body(), "503 Service Unavailable\n");
    EXPECT_TRUE(unavailable.keep_alive());
    const auto timeout = lb::proxyError(http::status::gateway_timeout, false, true);
    EXPECT_EQ(timeout.result_int(), 504);
    EXPECT_TRUE(timeout.body().empty());
    EXPECT_FALSE(timeout.keep_alive());
}
} // namespace
