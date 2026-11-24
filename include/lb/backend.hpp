#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lb {

enum class BackendState { UNKNOWN, HEALTHY, UNHEALTHY, DRAINING };

struct BackendConfig {
    std::string id;
    std::string host;
    std::uint16_t port = 0;
};

/// Identity is immutable. Counters and state remain valid for requests holding an
/// old snapshot after removal. Only BackendRegistry may change routing state.
struct BackendServer {
    BackendServer(std::string backendId, std::string backendHost, std::uint16_t backendPort)
        : id(std::move(backendId)), host(std::move(backendHost)), port(backendPort) {
        if (id.empty() || host.empty() || port == 0) {
            throw std::invalid_argument("Backend requires a nonempty id/host and nonzero port");
        }
    }

    const std::string id;
    const std::string host;
    const std::uint16_t port;
    std::atomic<std::uint64_t> requests{0};
    std::atomic<std::uint64_t> failures{0};
    // Fixed metric vocabularies: success/http_error/transport_error/timeout/cancelled;
    // resolve/connect/timeout/reset/protocol/transport; enum state order below.
    std::array<std::atomic<std::uint64_t>, 5> requestResults{};
    std::array<std::atomic<std::uint64_t>, 6> upstreamErrors{};
    std::array<std::atomic<std::uint64_t>, 4> stateChanges{};
    std::atomic<std::uint64_t> openConnections{0};
    std::atomic<std::uint64_t> inFlight{0};
    // Active readiness measurements are independent of real-traffic counters.
    std::atomic<std::uint64_t> probes{0};
    std::atomic<std::uint64_t> probeSuccesses{0};
    std::atomic<std::uint64_t> probeFailures{0};
    std::atomic<std::uint64_t> probeTimeouts{0};
    std::atomic<std::uint64_t> lastProbeTimeMs{0};
    std::atomic<std::uint64_t> lastTransitionTimeMs{0};

    [[nodiscard]] BackendState state() const noexcept {
        return m_state.load(std::memory_order_acquire);
    }

private:
    friend class BackendRegistry;
    std::atomic<BackendState> m_state{BackendState::UNKNOWN};
};

using BackendPtr = std::shared_ptr<BackendServer>;
using ServerList = std::vector<BackendPtr>;
using ServerListSnapshot = std::shared_ptr<const ServerList>;

} // namespace lb
