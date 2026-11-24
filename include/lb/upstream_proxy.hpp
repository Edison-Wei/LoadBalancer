#pragma once

#include "lb/config.hpp"
#include "lb/proxy_http.hpp"

#include <boost/asio/awaitable.hpp>

#include <functional>
#include <memory>
#include <utility>

namespace lb {
enum class UpstreamError { NONE, BAD_GATEWAY, TIMEOUT, CANCELLED };
enum class UpstreamStage { RESOLVE, CONNECT, WRITE, READ };

/// Single-executor cancellation for health-check shutdown. Each probe owns a new
/// instance; cancellation remains set even if the operation has not started yet.
class UpstreamCancellation {
public:
    void cancel() {
        m_cancelled = true;
        if (m_action) {
            m_action();
        }
    }
    [[nodiscard]] bool cancelled() const noexcept { return m_cancelled; }
    void setAction(std::function<void()> action) {
        m_action = std::move(action);
        if (m_cancelled && m_action) {
            m_action();
        }
    }
private:
    bool m_cancelled = false;
    std::function<void()> m_action;
};
enum class UpstreamReason { RESOLVE, CONNECT, TIMEOUT, RESET, PROTOCOL, TRANSPORT };
struct UpstreamResult {
    ProxyResponse response;
    UpstreamError error = UpstreamError::NONE;
    UpstreamStage stage = UpstreamStage::READ;
    bool overallTimeout = false;
    UpstreamReason reason = UpstreamReason::TRANSPORT;
};

boost::asio::awaitable<UpstreamResult> forwardToBackend(ProxyRequest request, BackendPtr backend,
                                                     const ProxyOptions& options,
                                                     unsigned responseTimeoutMs = 0,
                                                     std::shared_ptr<UpstreamCancellation> cancellation = {});
} // namespace lb
