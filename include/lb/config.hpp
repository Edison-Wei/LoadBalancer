#pragma once

#include "lb/backend.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lb {

struct HealthOptions {
    std::string path = "/health";
    unsigned intervalMs = 2000;
    unsigned connectTimeoutMs = 1000;
    unsigned responseTimeoutMs = 2000;
    unsigned failureThreshold = 3;
    unsigned successThreshold = 2;
};

void validateHealthOptions(const HealthOptions& options);

struct ProxyOptions {
    std::string listen = "127.0.0.1";
    std::uint16_t port = 8080;
    std::string metricsListen = "127.0.0.1";
    std::uint16_t metricsPort = 9091;
    std::vector<BackendConfig> backends;
    unsigned upstreamTimeoutMs = 3000;
    unsigned connectTimeoutMs = 1000;
    unsigned clientTimeoutMs = 10000;
    unsigned clientWriteTimeoutMs = 10000;
    unsigned writeTimeoutMs = 3000;
    unsigned readTimeoutMs = 3000;
    unsigned workers = 4;
    unsigned shutdownGraceMs = 5000;
    unsigned maxConnections = 4096;
    unsigned maxInFlight = 256;
    unsigned maxUpstreamConnections = 256;
    std::size_t maxBodyBytes = 1048576;
    std::uint32_t maxHeaderBytes = 16384;
    HealthOptions health;
    bool help = false;
    bool smoke = false;
};

ProxyOptions parseProxyOptions(std::span<const std::string_view> args);

} // namespace lb
