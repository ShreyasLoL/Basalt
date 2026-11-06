#pragma once

#include <filesystem>
#include <functional>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>

namespace basalt::server {

// Local JSON-RPC 2.0 / MCP transport. Paths are application-owned fixed paths;
// they are deliberately not configurable through the protocol.
class McpServer {
public:
    using Clock = std::function<long long()>;

    explicit McpServer(std::filesystem::path audit_path = "logs/mcp_audit.jsonl",
                       std::filesystem::path kill_log_path = "logs/mcp_killswitch.blog",
                       Clock clock = {});
    ~McpServer();
    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    // Processes one JSON-RPC frame and returns one response frame. Notifications
    // and shutdown return an empty string. Invalid input is handled as protocol
    // errors, never as an exception to the caller.
    [[nodiscard]] std::string process_line(const std::string& line);
    [[nodiscard]] bool run_stdio(std::istream& input, std::ostream& output);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace basalt::server
