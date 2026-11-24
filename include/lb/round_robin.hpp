#pragma once

#include "lb/backend.hpp"

#include <atomic>
#include <cstdint>

namespace lb {

/// Concurrent per-request scheduler. Selection uses only the supplied snapshot,
/// never a later state() value. Empty/null snapshots do not advance the sequence.
class RoundRobin {
  public:
    explicit RoundRobin(std::uint64_t initialSequence = 0) noexcept : m_sequence(initialSequence) {}

    [[nodiscard]] BackendPtr select(const ServerListSnapshot& snapshot) noexcept {
        if (!snapshot || snapshot->empty()) {
            return nullptr;
        }
        // Unsigned wraparound is defined. Counts are balanced for a fixed set
        // except for the possible discontinuity when the finite counter wraps.
        const auto sequence = m_sequence.fetch_add(1, std::memory_order_relaxed);
        return (*snapshot)[sequence % snapshot->size()];
    }

  private:
    std::atomic<std::uint64_t> m_sequence;
};

} // namespace lb
