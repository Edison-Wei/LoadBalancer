#include "lb/metrics.hpp"
#include "lb/client_session.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace lb {
namespace {
constexpr std::array<std::string_view, 8> METHODS{"GET",    "POST",  "HEAD",    "PUT",
                                                  "DELETE", "PATCH", "OPTIONS", "OTHER"};
constexpr std::array<double, 10> BOUNDS{0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 5};
constexpr std::array<std::string_view, 5> RESULTS{"success", "http_error", "transport_error",
                                                  "timeout", "cancelled"};
constexpr std::array<std::string_view, 6> ERRORS{"resolve", "connect",  "timeout",
                                                 "reset",   "protocol", "transport"};
constexpr std::array<std::string_view, 4> STATES{"unknown", "healthy", "unhealthy", "draining"};
std::string escaped(std::string_view value) {
    std::string result;
    for (const char ch : value) {
        if (ch == '\\' || ch == '"')
            result += '\\';
        if (ch == '\n')
            result += "\\n";
        else
            result += ch;
    }
    return result;
}
void metadata(std::ostream& out, std::string_view name, std::string_view type,
              std::string_view help) {
    out << "# HELP " << name << ' ' << help << "\n# TYPE " << name << ' ' << type << '\n';
}
} // namespace

void MetricsRegistry::completed(std::string_view method, unsigned status, double seconds) {
    if (status < 100 || status >= 600 || !std::isfinite(seconds) || seconds < 0)
        return;
    const auto found = std::find(METHODS.begin(), METHODS.end(), method);
    const auto index = found == METHODS.end() ? METHODS.size() - 1
                                              : static_cast<std::size_t>(found - METHODS.begin());
    const std::lock_guard lock(m_mutex);
    ++m_responses[index][status / 100 - 1];
    for (std::size_t bucket = 0; bucket < BOUNDS.size(); ++bucket) {
        if (seconds <= BOUNDS[bucket])
            ++m_buckets[bucket];
    }
    ++m_buckets.back();
    ++m_count;
    m_sum += seconds;
}

std::string MetricsRegistry::render(const ProxyState& state) const {
    decltype(m_responses) responses;
    decltype(m_buckets) buckets;
    std::uint64_t count;
    double sum;
    {
        const std::lock_guard lock(m_mutex);
        responses = m_responses;
        buckets = m_buckets;
        count = m_count;
        sum = m_sum;
    }
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(17);
    metadata(out, "lb_requests_total", "counter",
             "Downstream responses whose writes completed successfully.");
    for (std::size_t method = 0; method < METHODS.size(); ++method)
        for (std::size_t status = 0; status < 5; ++status)
            out << "lb_requests_total{method=\"" << METHODS[method] << "\",status_class=\""
                << status + 1 << "xx\"} " << responses[method][status] << '\n';
    metadata(out, "lb_request_duration_seconds", "histogram",
             "First received header byte through completed downstream write; buffered pipeline "
             "starts at processing.");
    for (std::size_t bucket = 0; bucket < buckets.size(); ++bucket) {
        out << "lb_request_duration_seconds_bucket{route=\"proxy\",le=\"";
        if (bucket < BOUNDS.size())
            out << BOUNDS[bucket];
        else
            out << "+Inf";
        out << "\"} " << buckets[bucket] << '\n';
    }
    out << "lb_request_duration_seconds_sum{route=\"proxy\"} " << sum
        << "\nlb_request_duration_seconds_count{route=\"proxy\"} " << count << '\n';
    metadata(out, "lb_inflight_requests", "gauge",
             "Admitted business requests including downstream writes.");
    out << "lb_inflight_requests " << state.requests.used() << '\n';
    metadata(out, "lb_active_connections", "gauge",
             "Established TCP sockets; upstream includes readiness probes.");
    const auto backends = state.registry->configuredBackends();
    std::uint64_t upstream = 0;
    for (const auto& backend : backends)
        upstream += backend->openConnections.load(std::memory_order_relaxed);
    out << "lb_active_connections{side=\"downstream\"} " << state.openDownstream.load()
        << "\nlb_active_connections{side=\"upstream\"} " << upstream << '\n';
    metadata(out, "lb_backend_healthy", "gauge", "Backend is in the eligible routing snapshot.");
    const auto eligible = state.registry->snapshot();
    for (const auto& backend : backends)
        out << "lb_backend_healthy{backend=\"" << escaped(backend->id) << "\"} "
            << (std::find(eligible->begin(), eligible->end(), backend) != eligible->end() ? 1 : 0)
            << '\n';
    metadata(out, "lb_backend_requests_total", "counter",
             "Completed upstream attempts by outcome; HTTP 4xx/5xx are http_error, cancellation is "
             "separate.");
    for (const auto& backend : backends)
        for (std::size_t result = 0; result < RESULTS.size(); ++result)
            out << "lb_backend_requests_total{backend=\"" << escaped(backend->id) << "\",result=\""
                << RESULTS[result] << "\"} " << backend->requestResults[result].load() << '\n';
    metadata(out, "lb_upstream_errors_total", "counter",
             "Business transport or protocol failures, excluding HTTP errors and cancellations.");
    for (const auto& backend : backends)
        for (std::size_t reason = 0; reason < ERRORS.size(); ++reason)
            out << "lb_upstream_errors_total{backend=\"" << escaped(backend->id) << "\",reason=\""
                << ERRORS[reason] << "\"} " << backend->upstreamErrors[reason].load() << '\n';
    metadata(out, "lb_healthcheck_total", "counter",
             "Readiness outcomes accepted for current backend identity.");
    for (const auto& backend : backends) {
        const auto label = "{backend=\"" + escaped(backend->id) + "\",result=\"";
        out << "lb_healthcheck_total" << label << "success\"} " << backend->probeSuccesses.load()
            << "\nlb_healthcheck_total" << label << "failure\"} " << backend->probeFailures.load()
            << '\n';
    }
    metadata(out, "lb_backend_state_changes_total", "counter",
             "Published backend transitions by destination state, including initial eligibility.");
    for (const auto& backend : backends)
        for (std::size_t target = 0; target < STATES.size(); ++target)
            out << "lb_backend_state_changes_total{backend=\"" << escaped(backend->id)
                << "\",to_state=\"" << STATES[target] << "\"} "
                << backend->stateChanges[target].load() << '\n';
    metadata(out, "lb_overload_rejections_total", "counter",
             "Business admission rejections; no rate limiter is implemented.");
    out << "lb_overload_rejections_total{reason=\"connection_limit\"} "
        << state.rejectedConnections.load()
        << "\nlb_overload_rejections_total{reason=\"request_limit\"} "
        << state.requestLimitRejections.load()
        << "\nlb_overload_rejections_total{reason=\"upstream_limit\"} "
        << state.upstreamLimitRejections.load()
        << "\nlb_overload_rejections_total{reason=\"shutdown\"} " << state.shutdownRejections.load()
        << '\n';
    return out.str();
}
} // namespace lb
