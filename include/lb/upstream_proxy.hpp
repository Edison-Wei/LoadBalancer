#pragma once

#include "lb/config.hpp"
#include "lb/proxy_http.hpp"

#include <boost/asio/awaitable.hpp>

namespace lb {
enum class UpstreamError { NONE, BAD_GATEWAY, TIMEOUT };
struct UpstreamResult {
    ProxyResponse response;
    UpstreamError error = UpstreamError::NONE;
};

boost::asio::awaitable<UpstreamResult> forwardToBackend(ProxyRequest request, BackendPtr backend,
                                                     const ProxyOptions& options);
} // namespace lb
