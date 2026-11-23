#pragma once

#include <string>

namespace lb {

struct FoundationResult {
    bool timerCompleted = false;
    std::string method;
    std::string target;
    std::string host;
    std::string body;
};

/// Runs a coroutine timer and parses a complete HTTP/1.1 request without opening sockets.
/// Each invocation owns its I/O context; no state is shared between callers.
[[nodiscard]] FoundationResult runFoundationSmoke();

} // namespace lb
