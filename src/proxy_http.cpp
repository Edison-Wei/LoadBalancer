#include "lb/proxy_http.hpp"

#include <boost/beast/core/string.hpp>

#include <string>
#include <vector>

namespace lb {
namespace {
namespace http = boost::beast::http;

bool token(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || std::string_view("!#$%&'*+-.^_`|~").find(value) != std::string_view::npos;
}

void appendVia(http::fields& fields) {
    const auto prior = fields[http::field::via];
    std::string value(prior.data(), prior.size());
    if (!value.empty()) {
        value += ", ";
    }
    value += "1.1 load-balancer";
    fields.set(http::field::via, value);
}
} // namespace

std::optional<http::status> validateClientHeader(const ProxyRequest& request) {
    if (request.version() != 11) {
        return http::status::http_version_not_supported;
    }
    if (request.method() == http::verb::connect || request.count(http::field::upgrade) != 0) {
        return http::status::not_implemented;
    }
    if (request.count(http::field::host) != 1 || request[http::field::host].empty() ||
        request.count(http::field::content_length) > 1 || request.count(http::field::transfer_encoding) > 1 ||
        (request.count(http::field::content_length) && request.count(http::field::transfer_encoding))) {
        return http::status::bad_request;
    }
    const auto target = request.target();
    if (target.empty() || target.front() != '/' || target.find('#') != boost::beast::string_view::npos) {
        return http::status::bad_request;
    }
    if (request.count(http::field::expect)) {
        return http::status::expectation_failed;
    }
    if (request.count(http::field::transfer_encoding) &&
        !boost::beast::iequals(request[http::field::transfer_encoding], "chunked")) {
        return http::status::not_implemented;
    }
    switch (request.method()) {
    case http::verb::get:
    case http::verb::post:
    case http::verb::head:
    case http::verb::put:
    case http::verb::delete_:
    case http::verb::patch:
    case http::verb::options:
        return std::nullopt;
    default:
        return http::status::method_not_allowed;
    }
}

bool stripHopByHop(http::fields& fields) {
    std::vector<std::string> nominated;
    for (const auto& field : fields) {
        if (field.name() != http::field::connection && field.name() != http::field::trailer) {
            continue;
        }
        const auto value = field.value();
        std::string_view remaining(value.data(), value.size());
        for (;;) {
            const auto comma = remaining.find(',');
            auto name = remaining.substr(0, comma);
            while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) {
                name.remove_prefix(1);
            }
            while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
                name.remove_suffix(1);
            }
            if (name.empty()) {
                return false;
            }
            for (const auto character : name) {
                if (!token(character)) {
                    return false;
                }
            }
            nominated.emplace_back(name);
            if (comma == std::string_view::npos) {
                break;
            }
            remaining.remove_prefix(comma + 1);
        }
    }
    for (const auto& name : nominated) {
        fields.erase(name);
    }
    for (const auto* name : {"Connection", "Keep-Alive", "Proxy-Connection", "Proxy-Authenticate",
                             "Proxy-Authorization", "TE", "Trailer", "Transfer-Encoding", "Upgrade"}) {
        fields.erase(name);
    }
    return true;
}

bool prepareUpstreamRequest(ProxyRequest& request, const BackendServer& backend,
                            std::string_view clientAddress) {
    if (!stripHopByHop(request.base())) {
        return false;
    }
    std::vector<std::string> forwarded;
    for (const auto& field : request) {
        const auto name = field.name_string();
        if (name.size() >= 12 && boost::beast::iequals(name.substr(0, 12), "X-Forwarded-")) {
            forwarded.emplace_back(name.data(), name.size());
        }
    }
    for (const auto& name : forwarded) {
        request.erase(name);
    }
    for (const auto* name : {"Forwarded", "X-Real-IP"}) {
        request.erase(name);
    }
    const auto host = backend.host.find(':') == std::string::npos ? backend.host : "[" + backend.host + "]";
    request.set(http::field::host, host + ":" + std::to_string(backend.port));
    request.set("X-Forwarded-For", std::string(clientAddress));
    request.set("X-Forwarded-Proto", "http");
    appendVia(request.base());
    request.keep_alive(false); // one upstream connection per request, no pooling/retry
    request.erase(http::field::content_length);
    request.prepare_payload();
    return true;
}

bool prepareClientResponse(ProxyResponse& response, bool keepAlive, bool head) {
    if (response.count(http::field::transfer_encoding) &&
        !boost::beast::iequals(response[http::field::transfer_encoding], "chunked")) {
        return false;
    }
    if (!stripHopByHop(response.base())) {
        return false;
    }
    appendVia(response.base());
    response.version(11);
    response.keep_alive(keepAlive);
    if (!head && response.result() != http::status::not_modified) {
        response.erase(http::field::content_length);
        response.prepare_payload();
    }
    return true;
}

ProxyResponse proxyError(http::status status, bool keepAlive, bool head) {
    ProxyResponse response{status, 11};
    response.keep_alive(keepAlive);
    response.set(http::field::server, "load-balancer");
    response.set(http::field::content_type, "text/plain");
    response.set(http::field::via, "1.1 load-balancer");
    if (!head) {
        const auto reason = response.reason();
        response.body() = std::to_string(response.result_int()) + " " + std::string(reason.data(), reason.size()) + "\n";
    }
    if (status == http::status::method_not_allowed) {
        response.set(http::field::allow, "GET, POST, HEAD, PUT, DELETE, PATCH, OPTIONS");
    }
    response.prepare_payload();
    return response;
}
} // namespace lb
