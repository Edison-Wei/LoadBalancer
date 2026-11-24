#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/beast/http.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace lb::simulator {

inline constexpr std::size_t MAX_BODY_BYTES = 1024 * 1024;
inline constexpr std::uint32_t MAX_HEADER_BYTES = 8192;
inline constexpr unsigned MAX_DELAY_MS = 5000;

struct Options {
    std::string id = "A";
    std::string listen = "127.0.0.1";
    std::uint16_t port = 9000;
    bool testMode = false;
    bool ready = true;
    bool help = false;
};

using Request = boost::beast::http::request<boost::beast::http::string_body>;
using Response = boost::beast::http::response<boost::beast::http::string_body>;

Options parseOptions(std::span<const std::string_view> args);
void validateOptions(const Options& options);

/// Confined to one io_context worker. Each connection may run a coroutine, but
/// methods/state must not be accessed from other threads. No network I/O here.
class Backend {
  public:
    explicit Backend(Options options);
    boost::asio::awaitable<Response> respond(Request request);

  private:
    Options m_options;
    bool m_ready;
    std::uint64_t m_sequence = 0;
};

/// Runs the standalone listener until SIGINT/SIGTERM. Port zero selects an OS
/// assigned port. Prints a flushed JSON listening event with the actual port.
void run(const Options& options);

} // namespace lb::simulator
