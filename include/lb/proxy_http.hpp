#pragma once

#include "lb/backend.hpp"

#include <boost/beast/http.hpp>

#include <optional>
#include <string_view>

namespace lb {
using ProxyRequest = boost::beast::http::request<boost::beast::http::string_body>;
using ProxyResponse = boost::beast::http::response<boost::beast::http::string_body>;

std::optional<boost::beast::http::status> validateClientHeader(const ProxyRequest& request);
bool stripHopByHop(boost::beast::http::fields& fields);
bool prepareUpstreamRequest(ProxyRequest& request, const BackendServer& backend,
                            std::string_view clientAddress);
bool prepareClientResponse(ProxyResponse& response, bool keepAlive, bool head);
ProxyResponse proxyError(boost::beast::http::status status, bool keepAlive = false, bool head = false);
} // namespace lb
