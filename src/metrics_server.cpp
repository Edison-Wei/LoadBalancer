#include "lb/metrics_server.hpp"
#include "lb/client_session.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

#include <chrono>

namespace lb {
namespace asio = boost::asio;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;

MetricsServer::MetricsServer(asio::any_io_executor executor, std::shared_ptr<ProxyState> state)
    : m_acceptor(executor), m_state(std::move(state)) {}

void MetricsServer::start(std::function<void(std::exception_ptr)> onError) {
    const tcp::endpoint endpoint(asio::ip::make_address(m_state->options.metricsListen), m_state->options.metricsPort);
    m_acceptor.open(endpoint.protocol());
    m_acceptor.set_option(asio::socket_base::reuse_address(true));
    m_acceptor.bind(endpoint);
    m_acceptor.listen(8);
    asio::co_spawn(m_acceptor.get_executor(), accept(),
        [self = shared_from_this(), onError = std::move(onError)](std::exception_ptr error) {
            if (error) onError(error);
        });
}

unsigned MetricsServer::port() const { return m_acceptor.local_endpoint().port(); }

asio::awaitable<void> MetricsServer::accept() {
    while (!m_stopped) {
        boost::system::error_code error;
        auto socket = co_await m_acceptor.async_accept(asio::redirect_error(asio::use_awaitable, error));
        if (m_stopped) co_return;
        if (error) throw boost::system::system_error(error);
        if (m_sessions.size() >= 8) {
            socket.close(error);
            continue;
        }
        const auto id = ++m_next;
        auto stream = std::make_shared<boost::beast::tcp_stream>(std::move(socket));
        m_sessions.emplace(id, stream);
        asio::co_spawn(m_acceptor.get_executor(), serve(stream),
            [self = shared_from_this(), stream, id](std::exception_ptr) {
                stream->close();
                self->m_sessions.erase(id);
            });
    }
}

asio::awaitable<void> MetricsServer::serve(std::shared_ptr<boost::beast::tcp_stream> stream) {
    boost::beast::flat_buffer buffer(8192);
    http::request_parser<http::empty_body> parser;
    parser.header_limit(8192);
    parser.body_limit(0);
    stream->expires_after(std::chrono::seconds(5));
    boost::system::error_code error;
    co_await http::async_read_header(*stream, buffer, parser, asio::redirect_error(asio::use_awaitable, error));
    if (error || m_stopped) co_return;
    const auto& request = parser.get();
    auto status = http::status::ok;
    if (request.chunked() || (parser.content_length() && *parser.content_length() != 0)) status = http::status::bad_request;
    else if (request.target() != "/metrics") status = http::status::not_found;
    else if (request.method() != http::verb::get) status = http::status::method_not_allowed;
    http::response<http::string_body> response(status, 11);
    response.keep_alive(false);
    response.set(http::field::content_type, "text/plain; version=0.0.4; charset=utf-8");
    if (status == http::status::method_not_allowed) response.set(http::field::allow, "GET");
    response.body() = status == http::status::ok ? m_state->metrics.render(*m_state) : std::string(response.reason()) + "\n";
    response.prepare_payload();
    stream->expires_after(std::chrono::seconds(5));
    co_await http::async_write(*stream, response, asio::redirect_error(asio::use_awaitable, error));
}

void MetricsServer::stop() {
    if (m_stopped) return;
    m_stopped = true;
    boost::system::error_code ignored;
    m_acceptor.cancel(ignored);
    m_acceptor.close(ignored);
    for (const auto& [id, stream] : m_sessions) {
        (void)id;
        stream->cancel();
        stream->close();
    }
}
} // namespace lb
