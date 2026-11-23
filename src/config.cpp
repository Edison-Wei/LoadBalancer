#include "lb/config.hpp"

#include <boost/asio/ip/address.hpp>

#include <charconv>
#include <stdexcept>
#include <unordered_set>

namespace lb {
namespace {
unsigned number(std::string_view text, unsigned maximum, bool allowZero = false) {
    unsigned result = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size() || result > maximum ||
        (!allowZero && result == 0)) {
        throw std::invalid_argument("Invalid or out-of-range numeric option");
    }
    return result;
}

bool identifier(std::string_view value) {
    if (value.empty() || value.size() > 64) {
        return false;
    }
    for (const char character : value) {
        if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '-' || character == '_' ||
              character == '.')) {
            return false;
        }
    }
    return true;
}
} // namespace

ProxyOptions parseProxyOptions(std::span<const std::string_view> args) {
    ProxyOptions options;
    std::unordered_set<std::string> ids;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const auto option = args[index];
        if (option == "--help") {
            options.help = true;
            continue;
        }
        if (option == "--smoke") {
            options.smoke = true;
            continue;
        }
        if (option != "--listen" && option != "--port" && option != "--backend" &&
            option != "--upstream-timeout-ms" && option != "--connect-timeout-ms" &&
            option != "--client-timeout-ms" && option != "--max-body-bytes" &&
            option != "--max-header-bytes") {
            throw std::invalid_argument("Unknown option: " + std::string(option));
        }
        if (++index == args.size()) {
            throw std::invalid_argument("Missing option value");
        }
        const auto value = args[index];
        if (option == "--listen") {
            options.listen = value;
        } else if (option == "--port") {
            options.port = static_cast<std::uint16_t>(number(value, 65535, true));
        } else if (option == "--backend") {
            const auto first = value.find(',');
            const auto last = value.rfind(',');
            if (first == std::string_view::npos || first == last || last + 1 == value.size()) {
                throw std::invalid_argument("Backend syntax: ID,HOST,PORT (unbracketed IPv6 allowed)");
            }
            BackendConfig backend{std::string(value.substr(0, first)),
                                  std::string(value.substr(first + 1, last - first - 1)),
                                  static_cast<std::uint16_t>(number(value.substr(last + 1), 65535))};
            if (!identifier(backend.id) || backend.host.empty() || backend.host.size() > 253 ||
                options.backends.size() >= 64 || !ids.insert(backend.id).second) {
                throw std::invalid_argument("Invalid/duplicate backend or more than 64 backends");
            }
            for (const unsigned char character : backend.host) {
                if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') || character == '-' || character == '_' ||
                      character == '.' || character == ':')) {
                    throw std::invalid_argument("Invalid backend host");
                }
            }
            options.backends.push_back(std::move(backend));
        } else if (option == "--upstream-timeout-ms") {
            options.upstreamTimeoutMs = number(value, 60000);
        } else if (option == "--connect-timeout-ms") {
            options.connectTimeoutMs = number(value, 60000);
        } else if (option == "--client-timeout-ms") {
            options.clientTimeoutMs = number(value, 60000);
        } else if (option == "--max-body-bytes") {
            options.maxBodyBytes = number(value, 16 * 1024 * 1024, true);
        } else {
            options.maxHeaderBytes = number(value, 65536);
        }
    }
    boost::system::error_code error;
    boost::asio::ip::make_address(options.listen, error);
    if (error) {
        throw std::invalid_argument("--listen must be a numeric IP address");
    }
    return options;
}

} // namespace lb
