#include "lb/client_session.hpp"
#include "lb/config.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <limits>
#include <thread>
#include <vector>

TEST(Metrics, CountsCompletedResponsesAndCumulativeFixedHistogram) {
    lb::ProxyState state(lb::ProxyOptions{});
    state.metrics.completed("GET", 200, 0.004);
    state.metrics.completed("GET", 503, 0.2);
    state.metrics.completed("arbitrary-user-method", 405, 7);
    state.metrics.completed("GET", 200, -1);
    state.metrics.completed("GET", 200, std::numeric_limits<double>::infinity());
    auto text = state.metrics.render(state);
    EXPECT_NE(text.find("lb_requests_total{method=\"GET\",status_class=\"2xx\"} 1\n"), std::string::npos);
    EXPECT_NE(text.find("lb_requests_total{method=\"OTHER\",status_class=\"4xx\"} 1\n"), std::string::npos);
    EXPECT_NE(text.find("le=\"0.0050000000000000001\"} 1\n"), std::string::npos);
    EXPECT_NE(text.find("le=\"0.25\"} 2\n"), std::string::npos);
    EXPECT_NE(text.find("le=\"+Inf\"} 3\n"), std::string::npos);
    EXPECT_NE(text.find("lb_request_duration_seconds_count{route=\"proxy\"} 3\n"), std::string::npos);
    EXPECT_EQ(text.find("arbitrary-user-method"), std::string::npos);
    EXPECT_EQ(text.back(), '\n');
}

TEST(Metrics, ConcurrentUpdatesAndScrapesPreserveCounts) {
    lb::ProxyState state(lb::ProxyOptions{});
    std::atomic<bool> done{false};
    std::jthread scraper([&] {
        while (!done.load()) {
            const auto text = state.metrics.render(state);
            EXPECT_NE(text.find("# TYPE lb_request_duration_seconds histogram"), std::string::npos);
        }
    });
    std::vector<std::jthread> workers;
    for (int worker = 0; worker < 8; ++worker)
        workers.emplace_back([&] { for (int i = 0; i < 1000; ++i) state.metrics.completed("POST", 201, 0.01); });
    for (auto& worker : workers) worker.join();
    done.store(true);
    scraper.join();
    const auto text = state.metrics.render(state);
    EXPECT_NE(text.find("method=\"POST\",status_class=\"2xx\"} 8000\n"), std::string::npos);
    EXPECT_NE(text.find("le=\"+Inf\"} 8000\n"), std::string::npos);
}

TEST(Metrics, EscapesIdentityAndReportsEligibilityTransitionsAndLiveResources) {
    lb::ProxyOptions options;
    options.backends.push_back({"a\"\\\nb", "127.0.0.1", 1});
    lb::ProxyState state(options);
    state.registry->setState(options.backends[0].id, lb::BackendState::HEALTHY);
    state.registry->setState(options.backends[0].id, lb::BackendState::HEALTHY);
    auto lease = state.requests.tryAcquire();
    const auto text = state.metrics.render(state);
    EXPECT_NE(text.find("lb_backend_healthy{backend=\"a\\\"\\\\\\nb\"} 1\n"), std::string::npos);
    EXPECT_NE(text.find("to_state=\"healthy\"} 1\n"), std::string::npos);
    EXPECT_NE(text.find("lb_inflight_requests 1\n"), std::string::npos);
    EXPECT_NE(text.find("result=\"success\"} 0\n"), std::string::npos);
}

TEST(MetricsConfig, DefaultsPrivateAndAcceptsEphemeralButRejectsInvalidAddresses) {
    const auto defaults = lb::parseProxyOptions({});
    EXPECT_EQ(defaults.metricsListen, "127.0.0.1");
    EXPECT_EQ(defaults.metricsPort, 9091);
    const std::vector<std::string_view> valid{"--metrics-listen", "::1", "--metrics-port", "0"};
    EXPECT_EQ(lb::parseProxyOptions(valid).metricsPort, 0);
    for (const auto& args : std::vector<std::vector<std::string_view>>{
             {"--metrics-listen", "localhost"}, {"--metrics-port", "65536"}, {"--metrics-port", "-1"}})
        EXPECT_THROW(lb::parseProxyOptions(args), std::invalid_argument);
}
