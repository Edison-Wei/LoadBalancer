#include "lb/registry.hpp"
#include "lb/round_robin.hpp"

#include <gtest/gtest.h>

#include <latch>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

const std::vector<lb::BackendConfig> BACKENDS = {{"A", "localhost", 9001}, {"B", "localhost", 9002}};

TEST(Registry, StartsFailClosedAndReportsUnknownIds) {
    lb::BackendRegistry registry(BACKENDS);
    ASSERT_NE(registry.snapshot(), nullptr);
    EXPECT_TRUE(registry.snapshot()->empty());
    const auto backend = registry.findBackend("A");
    ASSERT_NE(backend, nullptr);
    EXPECT_EQ(backend->state(), lb::BackendState::UNKNOWN);
    EXPECT_EQ(backend->id, "A");
    EXPECT_EQ(backend->host, "localhost");
    EXPECT_EQ(backend->port, 9001);
    EXPECT_EQ(backend->requests.load(), 0U);
    EXPECT_EQ(backend->failures.load(), 0U);
    EXPECT_EQ(backend->inFlight.load(), 0U);
    EXPECT_EQ(registry.findBackend("missing"), nullptr);
    EXPECT_FALSE(registry.setState("missing", lb::BackendState::HEALTHY));
}

TEST(Registry, PublishesOnlyHealthyAndAllowsRecovery) {
    lb::BackendRegistry registry(BACKENDS);
    ASSERT_TRUE(registry.setState("A", lb::BackendState::HEALTHY));
    ASSERT_EQ(registry.snapshot()->size(), 1U);
    for (const auto state : {lb::BackendState::UNHEALTHY, lb::BackendState::DRAINING,
                             lb::BackendState::UNKNOWN}) {
        ASSERT_TRUE(registry.setState("A", state));
        EXPECT_TRUE(registry.snapshot()->empty());
        ASSERT_TRUE(registry.setState("A", lb::BackendState::HEALTHY));
        ASSERT_EQ(registry.snapshot()->size(), 1U);
        EXPECT_EQ(registry.snapshot()->front()->id, "A");
    }
}

TEST(Registry, HeldSnapshotCanStillSelectRemovedBackendAndOwnsItsLifetime) {
    lb::BackendRegistry registry({BACKENDS.front()});
    ASSERT_TRUE(registry.setState("A", lb::BackendState::HEALTHY));
    auto snapshot = registry.snapshot();
    const std::weak_ptr<lb::BackendServer> old = snapshot->front();
    ASSERT_TRUE(registry.setState("A", lb::BackendState::DRAINING));
    registry.replaceBackends({});
    EXPECT_TRUE(registry.snapshot()->empty());
    EXPECT_EQ(registry.findBackend("A"), nullptr);
    lb::RoundRobin scheduler;
    auto selected = scheduler.select(snapshot);
    ASSERT_NE(selected, nullptr);
    EXPECT_EQ(selected->id, "A");
    EXPECT_EQ(selected->state(), lb::BackendState::DRAINING);
    snapshot.reset();
    EXPECT_FALSE(old.expired());
    selected.reset();
    EXPECT_TRUE(old.expired());
}

TEST(Registry, ReloadPreservesStateCountersIdentityAndUsesNewOrder) {
    lb::BackendRegistry registry(BACKENDS);
    ASSERT_TRUE(registry.setState("A", lb::BackendState::HEALTHY));
    ASSERT_TRUE(registry.setState("B", lb::BackendState::HEALTHY));
    const auto original = registry.findBackend("A");
    original->requests.fetch_add(7);
    original->failures.fetch_add(2);
    original->inFlight.fetch_add(1);
    registry.replaceBackends({BACKENDS[1], BACKENDS[0], {"C", "localhost", 9003}});
    EXPECT_EQ(registry.findBackend("A"), original);
    EXPECT_EQ(original->requests.load(), 7U);
    EXPECT_EQ(original->failures.load(), 2U);
    EXPECT_EQ(original->inFlight.load(), 1U);
    EXPECT_EQ(original->state(), lb::BackendState::HEALTHY);
    const auto snapshot = registry.snapshot();
    ASSERT_EQ(snapshot->size(), 2U);
    EXPECT_EQ((*snapshot)[0]->id, "B");
    EXPECT_EQ((*snapshot)[1]->id, "A");
    EXPECT_EQ(registry.findBackend("C")->state(), lb::BackendState::UNKNOWN);
}

TEST(Registry, ChangedEndpointStartsUnknownWithoutChangingOldIdentity) {
    lb::BackendRegistry registry({BACKENDS.front()});
    ASSERT_TRUE(registry.setState("A", lb::BackendState::HEALTHY));
    const auto old = registry.findBackend("A");
    registry.replaceBackends({{"A", "new-host", 9010}});
    const auto replacement = registry.findBackend("A");
    ASSERT_NE(replacement, nullptr);
    EXPECT_NE(replacement, old);
    EXPECT_EQ(replacement->state(), lb::BackendState::UNKNOWN);
    EXPECT_TRUE(registry.snapshot()->empty());
    EXPECT_EQ(old->host, "localhost");
    EXPECT_EQ(old->port, 9001);
    EXPECT_EQ(old->state(), lb::BackendState::HEALTHY);
}

TEST(Registry, InvalidReloadLeavesMembershipAndPublishedSnapshotIntact) {
    lb::BackendRegistry registry(BACKENDS);
    ASSERT_TRUE(registry.setState("A", lb::BackendState::HEALTHY));
    const auto before = registry.snapshot();
    for (const auto& invalid : std::vector<std::vector<lb::BackendConfig>>{
             {{"A", "localhost", 1}, {"A", "localhost", 2}},
             {{"", "localhost", 1}},
             {{"A", "", 1}},
             {{"A", "localhost", 0}}}) {
        EXPECT_THROW(registry.replaceBackends(invalid), std::invalid_argument);
        EXPECT_EQ(registry.snapshot(), before);
        EXPECT_EQ(registry.findBackend("A"), before->front());
        EXPECT_NE(registry.findBackend("B"), nullptr);
    }
    EXPECT_THROW(registry.setState("A", static_cast<lb::BackendState>(99)), std::invalid_argument);
    EXPECT_EQ(registry.snapshot(), before);
}

TEST(Registry, RR08SimultaneousStateChangeAndReloadCannotLoseUpdate) {
    // Each iteration races two writers from a shared starting point, then checks
    // the combined outcome. Both possible writer orders must preserve the change.
    for (int iteration = 0; iteration < 64; ++iteration) {
        lb::BackendRegistry registry(BACKENDS);
        ASSERT_TRUE(registry.setState("A", lb::BackendState::HEALTHY));
        ASSERT_TRUE(registry.setState("B", lb::BackendState::HEALTHY));
        const auto original = registry.findBackend("A");
        std::latch start(1);
        std::jthread health([&] {
            start.wait();
            registry.setState("A", lb::BackendState::UNHEALTHY);
        });
        std::jthread reload([&] {
            start.wait();
            registry.replaceBackends({BACKENDS[1], BACKENDS[0], {"C", "localhost", 9003}});
        });
        start.count_down();
        health.join();
        reload.join();
        EXPECT_EQ(registry.findBackend("A"), original);
        EXPECT_EQ(original->state(), lb::BackendState::UNHEALTHY);
        const auto snapshot = registry.snapshot();
        ASSERT_EQ(snapshot->size(), 1U) << "iteration " << iteration;
        EXPECT_EQ(snapshot->front()->id, "B");
        EXPECT_EQ(registry.findBackend("C")->state(), lb::BackendState::UNKNOWN);
    }
}

} // namespace
