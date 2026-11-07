#pragma once

#include <filesystem>
#include <functional>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>

namespace basalt::server {

// Local JSON-RPC 2.0 / MCP transport. Paths are application-owned fixed paths;
// they are deliberately not configurable through the protocol.
class McpServer {
public:
    using Clock = std::function<long long()>;

    // Resolves the default MCP data directory without depending on cwd:
    // 1. BASALT_DATA_DIR environment variable (or env_dir_override when supplied),
    // 2. directory of the running executable (or exe_dir_override when supplied),
    // 3. OS temporary directory (emitting a warning to warning_stream / std::cerr).
    // Ensures the chosen directory exists with owner-only permissions.
    [[nodiscard]] static std::filesystem::path resolve_data_dir(
        std::optional<std::filesystem::path> env_dir_override = std::nullopt,
        std::optional<std::filesystem::path> exe_dir_override = std::nullopt,
        std::ostream* warning_stream = nullptr);

    explicit McpServer(std::filesystem::path audit_path = {},
                       std::filesystem::path kill_log_path = {},
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
