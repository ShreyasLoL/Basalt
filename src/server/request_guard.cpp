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

RequestDecision deny(const int status, std::string code, std::string message) {
    return RequestDecision{status, std::move(code), std::move(message)};
}

} // namespace

std::vector<std::string> allowed_authorities(const int port) {
    const std::string suffix = ":" + std::to_string(port);
    return {"127.0.0.1" + suffix, "localhost" + suffix, "[::1]" + suffix};
}

bool is_allowed_host(const std::string_view host, const int port) {
    const std::string normalized = lowercase(trim(host));
    if (normalized.empty()) return false;
    for (const auto& authority : allowed_authorities(port)) {
        if (normalized == authority) return true;
        // Browsers omit the default port from Host; accept that form only on port 80.
        if (port == 80 && normalized + ":80" == authority) return true;
    }
    return false;
}

bool is_allowed_origin(const std::string_view origin, const int port,
                       const std::span<const std::string> allowlist) {
    const std::string normalized = lowercase(trim(origin));
    constexpr std::string_view http = "http://";
    constexpr std::string_view https = "https://";
    const auto scheme_size = normalized.starts_with(http) ? http.size()
        : (normalized.starts_with(https) ? https.size() : 0U);
    if (scheme_size == 0 || scheme_size == normalized.size()) return false;
    const std::string_view parsed_authority = std::string_view(normalized).substr(scheme_size);
    if (parsed_authority.find_first_of("/*?#@\\ \t\r\n") != std::string_view::npos) return false;

    for (const auto& allowed_origin : allowlist) {
        if (normalized == lowercase(trim(allowed_origin))) return true;
    }
    // The built-in same-origin set stays loopback-only and HTTP-only. Explicit
    // origins (for example a local Vite dev server) must be configured separately.
    if (!normalized.starts_with(http)) return false;
    const std::string_view authority = parsed_authority;
    for (const auto& allowed : allowed_authorities(port)) {
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
                                        const std::span<const std::string> allowlist) {
    if (!host || !is_allowed_host(*host, port)) {
        return deny(403, "FORBIDDEN_HOST",
                    "Host header must name the local BASALT gateway (loopback host:port)");
    }
    if (origin && !is_allowed_origin(*origin, port, allowlist)) {
        return deny(403, "FORBIDDEN_ORIGIN", "Cross-origin requests are not permitted");
    }
    if (method_requires_json(method) && (!content_type || !is_json_content_type(*content_type))) {
        return deny(415, "UNSUPPORTED_MEDIA_TYPE", "State-changing requests require Content-Type: application/json");
    }
    return RequestDecision{};
}

} // namespace basalt::server
