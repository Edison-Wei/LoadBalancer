#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace lb {
/// Non-waiting bounded admission. Leases remain safe if the limiter is destroyed
/// first, and return capacity exactly once even across moves/exceptions.
class ResourceLimiter {
    struct State {
        explicit State(std::size_t maximum) : limit(maximum) {}
        const std::size_t limit;
        std::atomic<std::size_t> used{0};
        std::atomic<std::size_t> peak{0};
    };
public:
    class Lease {
    public:
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept : m_state(std::move(other.m_state)) {}
        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                release();
                m_state = std::move(other.m_state);
            }
            return *this;
        }
        ~Lease() { release(); }
    private:
        friend class ResourceLimiter;
        explicit Lease(std::shared_ptr<State> state) : m_state(std::move(state)) {}
        void release() noexcept {
            if (m_state) {
                m_state->used.fetch_sub(1, std::memory_order_relaxed);
                m_state.reset();
            }
        }
        std::shared_ptr<State> m_state;
    };

    explicit ResourceLimiter(std::size_t limit) : m_state(std::make_shared<State>(limit)) {
        if (!limit) {
            throw std::invalid_argument("Resource limit must be positive");
        }
    }
    [[nodiscard]] std::optional<Lease> tryAcquire() {
        auto used = m_state->used.load(std::memory_order_relaxed);
        while (used < m_state->limit) {
            if (m_state->used.compare_exchange_weak(used, used + 1, std::memory_order_relaxed)) {
                auto peak = m_state->peak.load(std::memory_order_relaxed);
                while (peak < used + 1 && !m_state->peak.compare_exchange_weak(
                           peak, used + 1, std::memory_order_relaxed)) {}
                return Lease(m_state);
            }
        }
        return std::nullopt;
    }
    [[nodiscard]] std::size_t used() const noexcept { return m_state->used.load(std::memory_order_relaxed); }
    [[nodiscard]] std::size_t peak() const noexcept { return m_state->peak.load(std::memory_order_relaxed); }
private:
    std::shared_ptr<State> m_state;
};
} // namespace lb
