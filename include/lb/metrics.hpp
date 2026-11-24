#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace lb {
struct ProxyState;

/// Fixed series only. The short histogram lock keeps buckets/sum/count coherent;
/// rendering and socket I/O happen outside it. Unknown methods map to OTHER.
class MetricsRegistry {
  public:
    void completed(std::string_view method, unsigned status, double seconds);
    [[nodiscard]] std::string render(const ProxyState& state) const;

  private:
    mutable std::mutex m_mutex;
    std::array<std::array<std::uint64_t, 5>, 8> m_responses{};
    std::array<std::uint64_t, 11> m_buckets{};
    std::uint64_t m_count = 0;
    double m_sum = 0;
};
} // namespace lb
