#include "lb/config.hpp"
#include "lb/log.hpp"
#include "lb/resource_limiter.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <latch>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
TEST(ResourceLimiter, RejectsExcessAndReturnsMovedCapacityExactlyOnce) {
    lb::ResourceLimiter limiter(2);
    auto first = limiter.tryAcquire();
    auto second = limiter.tryAcquire();
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_FALSE(limiter.tryAcquire());
    std::optional<lb::ResourceLimiter::Lease> moved(std::move(*first));
    first.reset();
    EXPECT_EQ(limiter.used(), 2U);
    moved.reset();
    EXPECT_EQ(limiter.used(), 1U);
    second.reset();
    EXPECT_EQ(limiter.used(), 0U);
    EXPECT_EQ(limiter.peak(), 2U);
}

TEST(ResourceLimiter, LeasesSurviveOwnerAndReleaseDuringExceptionUnwinding) {
    EXPECT_THROW(lb::ResourceLimiter(0), std::invalid_argument);
    auto limiter = std::make_unique<lb::ResourceLimiter>(1);
    auto lease = limiter->tryAcquire();
    limiter.reset();
    lease.reset(); // shared counter ownership avoids use-after-free
    lb::ResourceLimiter bounded(1);
    try {
        auto scoped = bounded.tryAcquire();
        ASSERT_TRUE(scoped);
        throw std::runtime_error("test");
    } catch (const std::runtime_error&) {
    }
    EXPECT_EQ(bounded.used(), 0U);
    EXPECT_TRUE(bounded.tryAcquire());
}

TEST(ResourceLimiter, ConcurrentAdmissionNeverExceedsCap) {
    lb::ResourceLimiter limiter(8);
    std::latch ready(8);
    std::latch release(1);
    std::atomic<bool> admitted = true;
    std::vector<std::jthread> workers;
    for (int index = 0; index < 8; ++index) {
        workers.emplace_back([&] {
            auto lease = limiter.tryAcquire();
            if (!lease)
                admitted.store(false);
            ready.count_down();
            release.wait();
        });
    }
    ready.wait();
    EXPECT_TRUE(admitted.load());
    EXPECT_EQ(limiter.used(), 8U);
    EXPECT_FALSE(limiter.tryAcquire());
    release.count_down();
    for (auto& worker : workers)
        worker.join();
    EXPECT_EQ(limiter.used(), 0U);
    EXPECT_EQ(limiter.peak(), 8U);
}

TEST(WorkerConfig, ParsesResourceAndLifecycleBounds) {
    const std::vector<std::string_view> args = {"--workers",
                                                "8",
                                                "--max-connections",
                                                "1000",
                                                "--max-inflight",
                                                "500",
                                                "--max-upstream-connections",
                                                "400",
                                                "--shutdown-grace-ms",
                                                "0",
                                                "--client-read-timeout-ms",
                                                "100",
                                                "--client-write-timeout-ms",
                                                "200",
                                                "--upstream-write-timeout-ms",
                                                "300",
                                                "--upstream-read-timeout-ms",
                                                "400"};
    const auto options = lb::parseProxyOptions(args);
    EXPECT_EQ(options.workers, 8U);
    EXPECT_EQ(options.maxConnections, 1000U);
    EXPECT_EQ(options.maxInFlight, 500U);
    EXPECT_EQ(options.maxUpstreamConnections, 400U);
    EXPECT_EQ(options.shutdownGraceMs, 0U);
    EXPECT_EQ(options.clientTimeoutMs, 100U);
    EXPECT_EQ(options.clientWriteTimeoutMs, 200U);
    EXPECT_EQ(options.writeTimeoutMs, 300U);
    EXPECT_EQ(options.readTimeoutMs, 400U);
    const std::vector<std::string_view> automatic = {"--workers", "0"};
    EXPECT_GE(lb::parseProxyOptions(automatic).workers, 1U);
    EXPECT_LE(lb::parseProxyOptions(automatic).workers, 64U);
    for (const auto& invalid :
         std::vector<std::vector<std::string_view>>{{"--workers", "65"},
                                                    {"--max-connections", "0"},
                                                    {"--max-inflight", "0"},
                                                    {"--max-upstream-connections", "65537"},
                                                    {"--shutdown-grace-ms", "60001"},
                                                    {"--upstream-write-timeout-ms", "0"},
                                                    {"--client-write-timeout-ms", "0"}}) {
        EXPECT_THROW(lb::parseProxyOptions(invalid), std::invalid_argument);
    }
}

TEST(WorkerLogging, ConcurrentEventsStayOnSeparateCompleteLines) {
    std::ostringstream captured;
    auto* previous = std::clog.rdbuf(captured.rdbuf());
    std::vector<std::jthread> workers;
    for (int thread = 0; thread < 8; ++thread) {
        workers.emplace_back([thread] {
            for (int index = 0; index < 100; ++index)
                lb::LogLine{} << "{\"thread\":" << thread << ",\"index\":" << index << "}";
        });
    }
    for (auto& worker : workers)
        worker.join();
    std::clog.rdbuf(previous);
    std::istringstream lines(captured.str());
    std::string line;
    const std::regex event(R"(\{"thread":[0-7],"index":[0-9]+\})");
    int count = 0;
    while (std::getline(lines, line)) {
        EXPECT_TRUE(std::regex_match(line, event)) << line;
        ++count;
    }
    EXPECT_EQ(count, 800);
}
} // namespace
