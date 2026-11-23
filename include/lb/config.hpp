#pragma once

#include "lb/backend.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lb {

struct ProxyOptions {
    std::string listen = "127.0.0.1";
    std::uint16_t port = 8080;
    std::vector<BackendConfig> backends;
    unsigned upstreamTimeoutMs = 3000;
    unsigned connectTimeoutMs = 1000;
    unsigned clientTimeoutMs = 10000;
    std::size_t maxBodyBytes = 1048576;
    std::uint32_t maxHeaderBytes = 16384;
    bool help = false;
    bool smoke = false;
};

ProxyOptions parseProxyOptions(std::span<const std::string_view> args);

} // namespace lb
