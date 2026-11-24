#include "lb/health_checker.hpp"
#include "lb/round_robin.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
namespace asio = boost::asio;
using State = lb::BackendState;

std::shared_ptr<lb::BackendRegistry> registry() {
    return std::make_shared<lb::BackendRegistry>(std::vector<lb::BackendConfig>{{"A", "localhost", 9001}});
}

void bootstrap(asio::io_context& context, const std::shared_ptr<lb::HealthChecker>& checker) {
    context.restart();
    auto done = asio::co_spawn(context, checker->bootstrap(), asio::use_future);
    context.run();
    done.get();
}

TEST(HealthState, UnknownIsExcludedUntilItsFirstSuccess) {
    lb::HealthProgress progress(3, 2);
    EXPECT_EQ(progress.observe(State::UNKNOWN, false), State::UNKNOWN);
    EXPECT_EQ(progress.observe(State::UNKNOWN, true), State::HEALTHY);
    EXPECT_EQ(progress.successes(), 1U);
    EXPECT_EQ(progress.failures(), 0U);
}

TEST(HealthState, HealthyExclusionRequiresThreeConsecutiveFailures) {
    lb::HealthProgress progress(3, 2);
    EXPECT_EQ(progress.observe(State::HEALTHY, false), State::HEALTHY);
    EXPECT_EQ(progress.observe(State::HEALTHY, false), State::HEALTHY);
    EXPECT_EQ(progress.observe(State::HEALTHY, false), State::UNHEALTHY);
    EXPECT_EQ(progress.failures(), 3U);
    EXPECT_EQ(progress.successes(), 0U);
}

TEST(HealthState, RecoveryRequiresTwoConsecutiveSuccessesAndFailureResetsStreak) {
    lb::HealthProgress progress(3, 2);
    EXPECT_EQ(progress.observe(State::UNHEALTHY, true), State::UNHEALTHY);
    EXPECT_EQ(progress.observe(State::UNHEALTHY, false), State::UNHEALTHY);
    EXPECT_EQ(progress.observe(State::UNHEALTHY, true), State::UNHEALTHY);
    EXPECT_EQ(progress.observe(State::UNHEALTHY, true), State::HEALTHY);
    EXPECT_EQ(progress.failures(), 0U);
}

TEST(HealthState, AlternatingResultsDoNotCauseStateChurn) {
    lb::HealthProgress healthy(3, 2);
    lb::HealthProgress unhealthy(3, 2);
    for (int iteration = 0; iteration < 100; ++iteration) {
        EXPECT_EQ(healthy.observe(State::HEALTHY, false), State::HEALTHY);
        EXPECT_EQ(healthy.observe(State::HEALTHY, true), State::HEALTHY);
        EXPECT_EQ(unhealthy.observe(State::UNHEALTHY, true), State::UNHEALTHY);
        EXPECT_EQ(unhealthy.observe(State::UNHEALTHY, false), State::UNHEALTHY);
    }
}

TEST(HealthState, DrainingIsProtectedAndCountersSaturate) {
    EXPECT_THROW(lb::HealthProgress(0, 2), std::invalid_argument);
    EXPECT_THROW(lb::HealthProgress(3, 0), std::invalid_argument);
    lb::HealthProgress progress(3, 2);
    EXPECT_EQ(progress.observe(State::DRAINING, true), State::DRAINING);
    EXPECT_EQ(progress.observe(State::DRAINING, false), State::DRAINING);
    EXPECT_EQ(progress.successes(), 0U);
    EXPECT_EQ(progress.failures(), 0U);
    for (int iteration = 0; iteration < 10000; ++iteration) {
        progress.observe(State::UNHEALTHY, false);
    }
    EXPECT_EQ(progress.failures(), 3U);
    lb::HealthProgress immediate(1, 1);
    EXPECT_EQ(immediate.observe(State::UNKNOWN, false), State::UNHEALTHY);
    EXPECT_EQ(immediate.observe(State::UNHEALTHY, true), State::HEALTHY);
}

TEST(HealthConfig, ParsesPolicyAndRejectsInvalidThresholdsPathsAndDeadlines) {
    const std::vector<std::string_view> args = {
        "--health-path", "/ready", "--health-interval-ms", "50", "--health-connect-timeout-ms", "20",
        "--health-response-timeout-ms", "40", "--health-failure-threshold", "4", "--health-success-threshold", "3"};
    const auto options = lb::parseProxyOptions(args).health;
    EXPECT_EQ(options.path, "/ready");
    EXPECT_EQ(options.intervalMs, 50U);
    EXPECT_EQ(options.connectTimeoutMs, 20U);
    EXPECT_EQ(options.responseTimeoutMs, 40U);
    EXPECT_EQ(options.failureThreshold, 4U);
    EXPECT_EQ(options.successThreshold, 3U);
    for (const auto& invalid : std::vector<std::vector<std::string_view>>{
             {"--health-path", "http://example/"}, {"--health-path", "/bad path"},
             {"--health-path", "/health#fragment"}, {"--health-interval-ms", "0"},
             {"--health-response-timeout-ms", "0"}, {"--health-connect-timeout-ms", "60001"},
             {"--health-success-threshold", "0"}, {"--health-failure-threshold", "1001"}}) {
        EXPECT_THROW(lb::parseProxyOptions(invalid), std::invalid_argument);
    }
}

TEST(RegistryHealth, RejectsStaleEndpointsChangedStatesAndDraining) {
    const auto backends = registry();
    const auto old = backends->findBackend("A");
    backends->replaceBackends({{"A", "new-host", 9002}});
    EXPECT_FALSE(backends->setStateIfCurrent(old, State::UNKNOWN, State::HEALTHY));
    const auto current = backends->findBackend("A");
    backends->setState("A", State::DRAINING);
    EXPECT_FALSE(backends->setStateIfCurrent(current, State::UNKNOWN, State::HEALTHY));
    EXPECT_FALSE(backends->setStateIfCurrent(current, State::DRAINING, State::HEALTHY));
    EXPECT_TRUE(backends->snapshot()->empty());
    backends->setState("A", State::UNKNOWN);
    EXPECT_TRUE(backends->setStateIfCurrent(current, State::UNKNOWN, State::HEALTHY));
    EXPECT_EQ(backends->snapshot()->front(), current);
}

TEST(HealthChecker, BootstrapMakesAllHealthyWithoutConsumingSchedulerOrTrafficCounters) {
    asio::io_context context;
    auto backends = std::make_shared<lb::BackendRegistry>(std::vector<lb::BackendConfig>{
        {"A", "localhost", 9001}, {"B", "localhost", 9002}, {"C", "localhost", 9003}});
    backends->findBackend("A")->requests.store(7);
    lb::HealthChecker::Probe probe = [](lb::BackendPtr, std::shared_ptr<lb::UpstreamCancellation>)
        -> asio::awaitable<lb::ProbeOutcome> { co_return lb::ProbeOutcome{}; };
    const auto checker = std::make_shared<lb::HealthChecker>(context.get_executor(), backends, lb::HealthOptions{}, probe);
    EXPECT_THROW(checker->start(), std::logic_error);
    bootstrap(context, checker);
    ASSERT_EQ(backends->snapshot()->size(), 3U);
    lb::RoundRobin scheduler;
    for (const auto* id : {"A", "B", "C"}) {
        const auto backend = scheduler.select(backends->snapshot());
        EXPECT_EQ(backend->id, id);
        EXPECT_EQ(backend->probes.load(), 1U);
        EXPECT_EQ(backend->probeSuccesses.load(), 1U);
        EXPECT_GT(backend->lastProbeTimeMs.load(), 0U);
        EXPECT_GT(backend->lastTransitionTimeMs.load(), 0U);
        EXPECT_EQ(backend->failures.load(), 0U);
    }
    EXPECT_EQ(backends->findBackend("A")->requests.load(), 7U);
    checker->stop();
}

TEST(HealthChecker, TimeoutProbeCountersStaySeparateFromBusinessFailures) {
    asio::io_context context;
    const auto backends = registry();
    lb::HealthOptions options;
    options.failureThreshold = 1;
    const auto probe = [](lb::BackendPtr, std::shared_ptr<lb::UpstreamCancellation>)
        -> asio::awaitable<lb::ProbeOutcome> { co_return lb::ProbeOutcome{lb::ProbeReason::TIMEOUT, 0}; };
    const auto checker = std::make_shared<lb::HealthChecker>(context.get_executor(), backends, options, probe);
    bootstrap(context, checker);
    const auto backend = backends->findBackend("A");
    EXPECT_EQ(backend->state(), State::UNHEALTHY);
    EXPECT_EQ(backend->probeFailures.load(), 1U);
    EXPECT_EQ(backend->probeTimeouts.load(), 1U);
    EXPECT_EQ(backend->failures.load(), 0U);
    EXPECT_EQ(backend->requests.load(), 0U);
    checker->stop();
}

TEST(HealthChecker, RecurringProbesNeverOverlapAndStoppedResultCannotPublish) {
    asio::io_context context;
    const auto backends = registry();
    std::weak_ptr<lb::HealthChecker> weak;
    int active = 0;
    int maximum = 0;
    int calls = 0;
    const auto probe = [&](lb::BackendPtr, std::shared_ptr<lb::UpstreamCancellation>)
        -> asio::awaitable<lb::ProbeOutcome> {
        maximum = std::max(maximum, ++active);
        ++calls;
        const auto executor = co_await asio::this_coro::executor;
        asio::steady_timer timer(executor, std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
        if (calls == 3) {
            weak.lock()->stop();
        }
        --active;
        co_return lb::ProbeOutcome{};
    };
    lb::HealthOptions options;
    options.intervalMs = 1;
    const auto checker = std::make_shared<lb::HealthChecker>(context.get_executor(), backends, options, probe);
    weak = checker;
    bootstrap(context, checker);
    context.restart();
    checker->start();
    checker->start(); // idempotent, never starts a second loop
    context.run();
    EXPECT_EQ(calls, 3);
    EXPECT_EQ(maximum, 1);
    EXPECT_EQ(active, 0);
    EXPECT_EQ(backends->findBackend("A")->probes.load(), 2U);
    checker->stop();
}

TEST(HealthChecker, StopCancelsPendingProbeAndSuppressesEvenLateSuccess) {
    asio::io_context context;
    const auto backends = registry();
    std::weak_ptr<lb::HealthChecker> weak;
    bool cancelled = false;
    const auto probe = [&](lb::BackendPtr, std::shared_ptr<lb::UpstreamCancellation> cancellation)
        -> asio::awaitable<lb::ProbeOutcome> {
        auto timer = std::make_shared<asio::steady_timer>(context, std::chrono::seconds(10));
        cancellation->setAction([timer] { timer->cancel(); });
        asio::post(context, [&] { weak.lock()->stop(); });
        boost::system::error_code error;
        co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, error));
        cancelled = cancellation->cancelled() && error == asio::error::operation_aborted;
        cancellation->setAction({});
        co_return lb::ProbeOutcome{}; // late result must not restore eligibility
    };
    const auto checker = std::make_shared<lb::HealthChecker>(context.get_executor(), backends, lb::HealthOptions{}, probe);
    weak = checker;
    bootstrap(context, checker);
    EXPECT_TRUE(cancelled);
    EXPECT_TRUE(backends->snapshot()->empty());
    EXPECT_EQ(backends->findBackend("A")->probes.load(), 0U);
    checker->start(); // no-op after stop
}

TEST(HealthChecker, ReloadDuringProbeCannotPublishAnOldEndpointResult) {
    asio::io_context context;
    const auto backends = registry();
    const auto old = backends->findBackend("A");
    const auto probe = [&](lb::BackendPtr, std::shared_ptr<lb::UpstreamCancellation>)
        -> asio::awaitable<lb::ProbeOutcome> {
        backends->replaceBackends({{"A", "new-host", 9002}});
        co_return lb::ProbeOutcome{};
    };
    const auto checker = std::make_shared<lb::HealthChecker>(context.get_executor(), backends, lb::HealthOptions{}, probe);
    bootstrap(context, checker);
    EXPECT_TRUE(backends->snapshot()->empty());
    EXPECT_EQ(backends->findBackend("A")->state(), State::UNKNOWN);
    EXPECT_EQ(old->probes.load(), 0U);
    checker->stop();
}

TEST(UpstreamCancellation, CancelBeforeStartReturnsWithoutOpeningAnOperation) {
    asio::io_context context;
    auto cancellation = std::make_shared<lb::UpstreamCancellation>();
    cancellation->cancel();
    lb::ProxyOptions options;
    lb::ProxyRequest request{boost::beast::http::verb::get, "/health", 11};
    auto done = asio::co_spawn(context, lb::forwardToBackend(std::move(request),
        std::make_shared<lb::BackendServer>("A", "localhost", 9001), options, 10, cancellation), asio::use_future);
    context.run();
    EXPECT_EQ(done.get().error, lb::UpstreamError::CANCELLED);
}
} // namespace
