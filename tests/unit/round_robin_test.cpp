#include "lb/registry.hpp"
#include "lb/round_robin.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <latch>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

const std::vector<lb::BackendConfig> BACKENDS = {
    {"A", "127.0.0.1", 9001}, {"B", "127.0.0.1", 9002}, {"C", "127.0.0.1", 9003}};

void makeHealthy(lb::BackendRegistry& registry) {
    for (const auto& backend : BACKENDS) {
        ASSERT_TRUE(registry.setState(backend.id, lb::BackendState::HEALTHY));
    }
}

TEST(RoundRobin, RR01CyclesInConfigurationOrder) {
    lb::BackendRegistry registry(BACKENDS);
    makeHealthy(registry);
    lb::RoundRobin scheduler;
    const auto snapshot = registry.snapshot();
    for (const auto* id : {"A", "B", "C", "A", "B", "C"}) {
        const auto selected = scheduler.select(snapshot);
        ASSERT_NE(selected, nullptr);
        EXPECT_EQ(selected->id, id);
    }
}

TEST(RoundRobin, RR02SingleBackendAlwaysSelected) {
    lb::BackendRegistry registry({BACKENDS.front()});
    ASSERT_TRUE(registry.setState("A", lb::BackendState::HEALTHY));
    lb::RoundRobin scheduler;
    const auto snapshot = registry.snapshot();
    for (int selection = 0; selection < 20; ++selection) {
        EXPECT_EQ(scheduler.select(snapshot), registry.findBackend("A"));
    }
}

TEST(RoundRobin, RR03EmptyAndNullSnapshotsReturnNoneWithoutAdvancing) {
    lb::RoundRobin scheduler;
    EXPECT_EQ(scheduler.select(nullptr), nullptr);
    EXPECT_EQ(scheduler.select(std::make_shared<const lb::ServerList>()), nullptr);
    lb::BackendRegistry registry(BACKENDS);
    EXPECT_EQ(scheduler.select(registry.snapshot()), nullptr);
    makeHealthy(registry);
    const auto first = scheduler.select(registry.snapshot());
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->id, "A");
    // Use a fresh empty call followed by the second selection to verify it consumes no turn.
    EXPECT_EQ(scheduler.select(nullptr), nullptr);
    EXPECT_EQ(scheduler.select(registry.snapshot())->id, "B");
}

TEST(RoundRobin, RR04OnlyEligibleBackendsAppearInCurrentSnapshot) {
    lb::BackendRegistry registry(BACKENDS);
    makeHealthy(registry);
    ASSERT_TRUE(registry.setState("A", lb::BackendState::UNHEALTHY));
    ASSERT_TRUE(registry.setState("B", lb::BackendState::DRAINING));
    const auto snapshot = registry.snapshot();
    ASSERT_EQ(snapshot->size(), 1U);
    lb::RoundRobin scheduler;
    EXPECT_EQ(scheduler.select(snapshot)->id, "C");
}

TEST(RoundRobin, RR05UnsignedCounterWrapsSafelyWithNonPowerOfTwoPool) {
    lb::BackendRegistry registry(BACKENDS);
    makeHealthy(registry);
    lb::RoundRobin scheduler(std::numeric_limits<std::uint64_t>::max() - 1);
    const auto snapshot = registry.snapshot();
    // uint64 max is divisible by three. Wrap is safe but may repeat one index.
    for (const auto* id : {"C", "A", "A", "B"}) {
        EXPECT_EQ(scheduler.select(snapshot)->id, id);
    }
}

TEST(RoundRobin, RR06ConcurrentSelectionsHaveExactAggregateCounts) {
    lb::BackendRegistry registry(BACKENDS);
    makeHealthy(registry);
    const auto snapshot = registry.snapshot();
    lb::RoundRobin scheduler;
    std::array<std::atomic<int>, 3> counts{};
    std::atomic<bool> valid = true;
    std::latch start(1);
    std::vector<std::jthread> workers;
    for (int worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&] {
            start.wait();
            for (int selection = 0; selection < 3000; ++selection) {
                const auto selected = scheduler.select(snapshot);
                const auto found = std::find(snapshot->begin(), snapshot->end(), selected);
                if (found == snapshot->end()) {
                    valid.store(false);
                } else {
                    counts[static_cast<std::size_t>(found - snapshot->begin())].fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
        });
    }
    start.count_down();
    for (auto& worker : workers) {
        worker.join();
    }
    ASSERT_TRUE(valid.load());
    for (const auto& count : counts) {
        EXPECT_EQ(count.load(), 8000);
    }
}

TEST(RoundRobin, RR07ConcurrentReplacementPreservesObservedSnapshotMembership) {
    lb::BackendRegistry registry(BACKENDS);
    makeHealthy(registry);
    lb::RoundRobin scheduler;
    std::atomic<bool> valid = true;
    std::latch start(1);
    std::vector<std::jthread> readers;
    for (int reader = 0; reader < 4; ++reader) {
        readers.emplace_back([&] {
            start.wait();
            for (int selection = 0; selection < 4000; ++selection) {
                const auto snapshot = registry.snapshot();
                const auto selected = scheduler.select(snapshot);
                if (snapshot->empty()) {
                    if (selected) {
                        valid.store(false);
                    }
                } else if (!selected ||
                           std::find(snapshot->begin(), snapshot->end(), selected) == snapshot->end()) {
                    valid.store(false);
                }
            }
        });
    }
    std::jthread writer([&] {
        start.wait();
        for (int update = 0; update < 1000; ++update) {
            if (update % 2 == 0) {
                registry.replaceBackends({BACKENDS[0], BACKENDS[1]});
                registry.setState("A", lb::BackendState::HEALTHY);
            } else {
                registry.replaceBackends({BACKENDS[1], BACKENDS[2]});
                registry.setState("C", lb::BackendState::HEALTHY);
            }
        }
    });
    start.count_down();
    writer.join();
    for (auto& reader : readers) {
        reader.join();
    }
    EXPECT_TRUE(valid.load());
}

} // namespace
