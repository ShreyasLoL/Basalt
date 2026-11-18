#include "basalt/server/mcp_server.hpp"

#include <filesystem>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) {
    if (size > 300U * 1024U) return 0;
    std::error_code error;
    const auto root = std::filesystem::temp_directory_path() / "basalt-phase4-fuzz-mcp";
    std::filesystem::remove_all(root, error);
    error.clear();
    std::filesystem::create_directories(root, error);
    if (error) return 0;
    static constexpr char empty[] = "";
    const auto* bytes = size == 0 ? empty : reinterpret_cast<const char*>(data);
    {
        basalt::server::McpServer server(root / "audit.jsonl", root / "killswitch.blog", [] { return 1LL; });
        (void)server.process_line(std::string(bytes, size));
    }
    std::filesystem::remove_all(root, error);
    return 0;
}
