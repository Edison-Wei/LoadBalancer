#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/tcp_stream.hpp>

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <unordered_map>

namespace lb {
struct ProxyState;
/// Confined to runtime control strand. At most eight monitoring sockets, 8 KiB
/// headers, no request bodies, one response per socket, five-second I/O deadlines.
class MetricsServer : public std::enable_shared_from_this<MetricsServer> {
public:
    MetricsServer(boost::asio::any_io_executor executor, std::shared_ptr<ProxyState> state);
    void start(std::function<void(std::exception_ptr)> onError);
    void stop();
    [[nodiscard]] unsigned port() const;
private:
    boost::asio::awaitable<void> accept();
    boost::asio::awaitable<void> serve(std::shared_ptr<boost::beast::tcp_stream> stream);
    boost::asio::ip::tcp::acceptor m_acceptor;
    std::shared_ptr<ProxyState> m_state;
    std::unordered_map<std::uint64_t, std::shared_ptr<boost::beast::tcp_stream>> m_sessions;
    std::uint64_t m_next = 0;
    bool m_stopped = false;
};
} // namespace lb
