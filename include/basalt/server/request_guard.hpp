#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::server {

// Shared with the MCP run_benchmark cap so both adapters bound work identically.
inline constexpr std::uint64_t MAX_HTTP_BENCHMARK_ORDER_COUNT = 10'000;

// Outcome of the transport-level request policy. status is 200 when the request
// may proceed to routing, otherwise 403 (host/origin) or 415 (media type).
struct RequestDecision {
    int status{200};
    std::string code{};
    std::string message{};
    [[nodiscard]] bool allowed() const noexcept { return status == 200; }
};

// Host-header authorities ("host:port", lowercase) that the gateway answers to.
// Always the loopback names for `port`; an explicitly configured non-wildcard,
// non-loopback bind address (remote opt-in) is added as an IP-literal authority.
[[nodiscard]] std::vector<std::string> allowed_authorities(int port,
                                                           std::string_view bind_address = "127.0.0.1");

// True when `host` names one of allowed_authorities(). Case-insensitive; an
// omitted port is accepted only when the server port is 80.
[[nodiscard]] bool is_allowed_host(std::string_view host, int port,
                                   std::string_view bind_address = "127.0.0.1");

// True when `origin` is exactly "http://" + one of allowed_authorities().
// "null", other schemes, other ports and other hostnames are rejected.
[[nodiscard]] bool is_allowed_origin(std::string_view origin, int port,
                                     std::string_view bind_address = "127.0.0.1");

// True when the media type (parameters ignored) is application/json.
[[nodiscard]] bool is_json_content_type(std::string_view content_type);

// POST, PUT, PATCH and DELETE must declare application/json. A non-simple
// content type forces browsers to send a CORS preflight before the mutation.
[[nodiscard]] bool method_requires_json(std::string_view method);

// Pure request policy applied before routing. Order: Host (403), Origin when
// present (403), Content-Type for state-changing methods (415). Absent headers
// are std::nullopt; a present-but-empty Origin is treated as a mismatch.
[[nodiscard]] RequestDecision validate_request_origin(std::optional<std::string_view> host,
                                                      std::optional<std::string_view> origin,
                                                      std::optional<std::string_view> content_type,
                                                      std::string_view method,
                                                      int port,
                                                      std::string_view bind_address = "127.0.0.1");

[[nodiscard]] constexpr bool valid_benchmark_order_count(const std::uint64_t count) noexcept {
    return count >= 1 && count <= MAX_HTTP_BENCHMARK_ORDER_COUNT;
}

} // namespace basalt::server
