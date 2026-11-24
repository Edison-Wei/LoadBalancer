#include "lb/client_session.hpp"
#include "lb/log.hpp"
#include "lb/proxy_http.hpp"

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>

#include <chrono>
#include <exception>
#include <utility>

namespace lb {
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

http::status parserStatus(const boost::system::error_code& error) {
    if (error == http::error::body_limit) return http::status::payload_too_large;
    if (error == http::error::header_limit) return http::status::request_header_fields_too_large;
    if (error == beast::error::timeout) return http::status::request_timeout;
    return http::status::bad_request;
}

const char* stageName(UpstreamStage stage) {
    switch (stage) {
    case UpstreamStage::RESOLVE: return "resolve";
    case UpstreamStage::CONNECT: return "connect";
    case UpstreamStage::WRITE: return "write";
    case UpstreamStage::READ: return "read";
    }
    return "invalid";
}

struct InFlight {
    explicit InFlight(BackendPtr value) : backend(std::move(value)) {
        backend->inFlight.fetch_add(1, std::memory_order_relaxed);
    }
    ~InFlight() { backend->inFlight.fetch_sub(1, std::memory_order_relaxed); }
    BackendPtr backend;
};
} // namespace

ClientSession::ClientSession(asio::ip::tcp::socket socket, std::shared_ptr<ProxyState> state,
                             ResourceLimiter::Lease connection, std::uint64_t id)
    : m_state(std::move(state)), m_connection(std::move(connection)), m_client(std::move(socket)), m_id(id) {
    m_state->openDownstream.fetch_add(1, std::memory_order_relaxed);
}

void ClientSession::stopWatching() {
    ++m_watchGeneration;
    if (m_watchSignal) {
        m_watchSignal->emit(asio::cancellation_type::all);
        m_watchSignal.reset();
    }
}

void ClientSession::watchDisconnect() {
    const auto generation = ++m_watchGeneration;
    auto signal = std::make_shared<asio::cancellation_signal>();
    m_watchSignal = signal;
    m_client.socket().async_wait(asio::ip::tcp::socket::wait_read,
        asio::bind_cancellation_slot(signal->slot(),
            [weak = weak_from_this(), signal, generation](boost::system::error_code error) {
                const auto self = weak.lock();
                if (!self || self->m_closed || generation != self->m_watchGeneration ||
                    error == asio::error::operation_aborted) return;
                if (!error) {
                    char byte;
                    self->m_client.socket().receive(asio::buffer(&byte, 1), asio::socket_base::message_peek, error);
                }
                if (error == asio::error::connection_reset || error == asio::error::connection_aborted) {
                    self->m_reset = true;
                    self->m_state->resetCancellations.fetch_add(1, std::memory_order_relaxed);
                    self->closeNow();
                }
                // FIN can be a legitimate half-close; queued data may be a
                // pipelined request. Never consume it or interpret either as RST.
                // In those cases, existing upstream deadlines remain the bound.
            }));
}

void ClientSession::closeNow() {
    if (m_closed) return;
    m_closed = true;
    m_state->openDownstream.fetch_sub(1, std::memory_order_relaxed);
    stopWatching();
    if (m_active) m_active->cancel();
    boost::system::error_code ignored;
    m_client.socket().cancel(ignored);
    m_client.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    m_client.socket().close(ignored);
}

void ClientSession::requestDrain() {
    asio::dispatch(m_client.get_executor(), [self = shared_from_this()] {
        self->m_draining = true;
        if (!self->m_requestActive) self->closeNow();
    });
}

void ClientSession::forceClose() {
    asio::dispatch(m_client.get_executor(), [self = shared_from_this()] { self->closeNow(); });
}

asio::awaitable<bool> ClientSession::writeResponse(ProxyResponse& response) {
    if (m_closed) co_return false;
    if (m_draining || m_state->stopping.load(std::memory_order_acquire)) response.keep_alive(false);
    m_client.expires_after(std::chrono::milliseconds(m_state->options.clientWriteTimeoutMs));
    boost::system::error_code error;
    co_await http::async_write(m_client, response, asio::redirect_error(asio::use_awaitable, error));
    if (error == beast::error::timeout) {
        LogLine{} << "{\"event\":\"client_write_timeout\",\"connection\":" << m_id << "}";
    }
    if (!error) {
        m_state->metrics.completed(m_method, response.result_int(),
            std::chrono::duration<double>(std::chrono::steady_clock::now() - m_requestStarted).count());
    }
    co_return !error;
}

asio::awaitable<void> ClientSession::run() {
    beast::flat_buffer buffer(m_state->options.maxBodyBytes + m_state->options.maxHeaderBytes);
    try {
        m_client.socket().non_blocking(true); // makes disconnect peeks nonblocking too
        const auto address = m_client.socket().remote_endpoint().address().to_string();
        while (!m_closed && !m_draining) {
            m_requestActive = false;
            http::request_parser<http::string_body> parser;
            parser.body_limit(m_state->options.maxBodyBytes);
            parser.header_limit(m_state->options.maxHeaderBytes);
            m_client.expires_after(std::chrono::milliseconds(m_state->options.clientTimeoutMs));
            boost::system::error_code error;
            // Wait for the first byte under the existing ingress deadline so
            // keep-alive idle time is excluded from response latency.
            if (buffer.size() == 0) {
                const auto received = co_await m_client.async_read_some(buffer.prepare(1),
                    asio::redirect_error(asio::use_awaitable, error));
                buffer.commit(received);
                if (error || m_closed) {
                    if (error == beast::error::timeout)
                        LogLine{} << "{\"event\":\"client_read_timeout\",\"connection\":" << m_id << "}";
                    break;
                }
            }
            m_requestStarted = std::chrono::steady_clock::now();
            co_await http::async_read_header(m_client, buffer, parser,
                                            asio::redirect_error(asio::use_awaitable, error));
            if (m_closed || error == http::error::end_of_stream) break;
            auto rejection = error ? std::optional<http::status>(parserStatus(error))
                                   : validateClientHeader(parser.get());
            m_method = std::string(parser.get().method_string());
            m_requestActive = true;
            if (rejection) {
                if (error == beast::error::timeout)
                    LogLine{} << "{\"event\":\"client_read_timeout\",\"connection\":" << m_id << "}";
                auto response = proxyError(*rejection, false, parser.get().method() == http::verb::head);
                co_await writeResponse(response);
                break;
            }
            auto requestLease = m_state->requests.tryAcquire();
            const bool stopping = m_state->stopping.load(std::memory_order_acquire);
            if (!requestLease || stopping) {
                m_state->rejectedRequests.fetch_add(1, std::memory_order_relaxed);
                if (stopping) m_state->shutdownRejections.fetch_add(1, std::memory_order_relaxed);
                else m_state->requestLimitRejections.fetch_add(1, std::memory_order_relaxed);
                LogLine{} << "{\"event\":\"request_rejected\",\"reason\":\""
                          << (stopping ? "shutdown" : "request_limit") << "\"}";
                auto response = proxyError(http::status::service_unavailable, false,
                                           parser.get().method() == http::verb::head);
                co_await writeResponse(response);
                break;
            }
            LogLine{} << "{\"event\":\"request_admitted\",\"connection\":" << m_id << "}";
            // The ingress deadline covers headers plus the entire body; partial
            // progress cannot keep extending a slow client's deadline.
            if (!parser.is_done()) {
                co_await http::async_read(m_client, buffer, parser,
                                         asio::redirect_error(asio::use_awaitable, error));
                if (m_closed) break;
                if (error) {
                    if (error == beast::error::timeout)
                        LogLine{} << "{\"event\":\"client_read_timeout\",\"connection\":" << m_id << "}";
                    auto response = proxyError(parserStatus(error), false, parser.get().method() == http::verb::head);
                    co_await writeResponse(response);
                    break;
                }
            }
            auto request = parser.release();
            const bool keepAlive = request.keep_alive();
            const bool head = request.method() == http::verb::head;
            const auto snapshot = m_state->registry->snapshot();
            const auto backend = m_state->scheduler.select(snapshot);
            ProxyResponse response;
            if (!backend) {
                response = proxyError(http::status::service_unavailable, keepAlive, head);
            } else if (!prepareUpstreamRequest(request, *backend, address)) {
                response = proxyError(http::status::bad_request, false, head);
            } else if (auto upstreamLease = m_state->upstreams.tryAcquire()) {
                InFlight flight(backend);
                backend->requests.fetch_add(1, std::memory_order_relaxed);
                m_active = std::make_shared<UpstreamCancellation>();
                watchDisconnect();
                auto upstream = co_await forwardToBackend(std::move(request), backend, m_state->options, 0, m_active);
                stopWatching();
                m_active.reset();
                if (upstream.error == UpstreamError::CANCELLED) {
                    backend->requestResults[4].fetch_add(1, std::memory_order_relaxed);
                    LogLine{} << "{\"event\":\"upstream_cancelled\",\"reason\":\""
                              << (m_reset ? "client_reset" : "shutdown") << "\",\"backend\":\"" << backend->id << "\"}";
                    break;
                }
                if (upstream.error != UpstreamError::NONE) {
                    backend->failures.fetch_add(1, std::memory_order_relaxed);
                    backend->requestResults[upstream.error == UpstreamError::TIMEOUT ? 3 : 2].fetch_add(1, std::memory_order_relaxed);
                    backend->upstreamErrors[static_cast<std::size_t>(upstream.reason)].fetch_add(1, std::memory_order_relaxed);
                    response = proxyError(upstream.error == UpstreamError::TIMEOUT
                                              ? http::status::gateway_timeout : http::status::bad_gateway,
                                          keepAlive, head);
                } else {
                    response = std::move(upstream.response);
                    if (!prepareClientResponse(response, keepAlive, head)) {
                        backend->failures.fetch_add(1, std::memory_order_relaxed);
                        backend->requestResults[2].fetch_add(1, std::memory_order_relaxed);
                        backend->upstreamErrors[4].fetch_add(1, std::memory_order_relaxed);
                        response = proxyError(http::status::bad_gateway, false, head);
                    } else {
                        backend->requestResults[response.result_int() >= 400 ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
                    }
                }
                LogLine{} << "{\"event\":\"upstream_complete\",\"backend\":\"" << backend->id
                          << "\",\"status\":" << response.result_int() << ",\"stage\":\"" << stageName(upstream.stage)
                          << "\",\"deadline\":\"" << (upstream.overallTimeout ? "overall" : "stage") << "\"}";
            } else {
                m_state->rejectedRequests.fetch_add(1, std::memory_order_relaxed);
                m_state->upstreamLimitRejections.fetch_add(1, std::memory_order_relaxed);
                LogLine{} << "{\"event\":\"request_rejected\",\"reason\":\"upstream_limit\"}";
                response = proxyError(http::status::service_unavailable, false, head);
            }
            if (!co_await writeResponse(response) || !response.keep_alive()) break;
            m_requestActive = false;
        }
    } catch (const std::exception&) {
        LogLine{} << "{\"event\":\"client_session_error\"}";
    }
    m_requestActive = false;
    closeNow();
}
} // namespace lb
