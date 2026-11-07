#include "basalt/server/request_guard.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

namespace basalt::server {
namespace {

std::string lowercase(const std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
}

bool is_loopback_or_wildcard(const std::string_view address) {
    constexpr std::array<std::string_view, 6> names{"127.0.0.1", "::1", "localhost", "0.0.0.0", "::", ""};
    return std::find(names.begin(), names.end(), address) != names.end();
}

RequestDecision deny(const int status, std::string code, std::string message) {
    return RequestDecision{status, std::move(code), std::move(message)};
}

} // namespace

std::vector<std::string> allowed_authorities(const int port, const std::string_view bind_address) {
    const std::string suffix = ":" + std::to_string(port);
    std::vector<std::string> result{"127.0.0.1" + suffix, "localhost" + suffix, "[::1]" + suffix};
    const std::string bind = lowercase(trim(bind_address));
    if (!is_loopback_or_wildcard(bind)) {
        const bool ipv6 = bind.find(':') != std::string::npos;
        result.push_back((ipv6 ? "[" + bind + "]" : bind) + suffix);
    }
    return result;
}

bool is_allowed_host(const std::string_view host, const int port, const std::string_view bind_address) {
    const std::string normalized = lowercase(trim(host));
    if (normalized.empty()) return false;
    for (const auto& authority : allowed_authorities(port, bind_address)) {
        if (normalized == authority) return true;
        // Browsers omit the default port from Host; accept that form only on port 80.
        if (port == 80 && normalized + ":80" == authority) return true;
    }
    return false;
}

bool is_allowed_origin(const std::string_view origin, const int port, const std::string_view bind_address) {
    const std::string normalized = lowercase(trim(origin));
    constexpr std::string_view scheme = "http://";
    if (normalized.size() <= scheme.size() || normalized.compare(0, scheme.size(), scheme) != 0) return false;
    const std::string_view authority = std::string_view(normalized).substr(scheme.size());
    if (authority.find('/') != std::string_view::npos) return false;
    for (const auto& allowed : allowed_authorities(port, bind_address)) {
        if (authority == allowed) return true;
        if (port == 80 && std::string(authority) + ":80" == allowed) return true;
    }
    return false;
}

bool is_json_content_type(const std::string_view content_type) {
    const auto separator = content_type.find(';');
    const auto media_type = trim(content_type.substr(0, separator));
    return lowercase(media_type) == "application/json";
}

bool method_requires_json(const std::string_view method) {
    return method == "POST" || method == "PUT" || method == "PATCH" || method == "DELETE";
}

RequestDecision validate_request_origin(const std::optional<std::string_view> host,
                                        const std::optional<std::string_view> origin,
                                        const std::optional<std::string_view> content_type,
                                        const std::string_view method,
                                        const int port,
                                        const std::string_view bind_address) {
    if (!host || !is_allowed_host(*host, port, bind_address)) {
        return deny(403, "FORBIDDEN_HOST",
                    "Host header must name the local BASALT gateway (loopback host:port)");
    }
    if (origin && !is_allowed_origin(*origin, port, bind_address)) {
        return deny(403, "FORBIDDEN_ORIGIN", "Cross-origin requests are not permitted");
    }
    if (method_requires_json(method) && (!content_type || !is_json_content_type(*content_type))) {
        return deny(415, "UNSUPPORTED_MEDIA_TYPE", "State-changing requests require Content-Type: application/json");
    }
    return RequestDecision{};
}

} // namespace basalt::server
