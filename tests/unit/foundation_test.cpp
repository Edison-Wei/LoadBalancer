#include "lb/atomic_shared_ptr.hpp"
#include "lb/foundation.hpp"

#include <gtest/gtest.h>

#include <memory>

TEST(Foundation, CompletesCoroutineTimerAndParsesHttpRequest) {
    const auto result = lb::runFoundationSmoke();
    EXPECT_TRUE(result.timerCompleted);
    EXPECT_EQ(result.method, "POST");
    EXPECT_EQ(result.target, "/smoke");
    EXPECT_EQ(result.host, "localhost");
    EXPECT_EQ(result.body, "hello");

    // Exercise the portable publication primitive used by the registry.
    lb::AtomicSharedPtr<const int> snapshot;
    snapshot.store(std::make_shared<const int>(42));
    const auto observed = snapshot.load();
    ASSERT_NE(observed, nullptr);
    EXPECT_EQ(*observed, 42);
}
