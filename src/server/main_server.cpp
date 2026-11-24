#include "basalt/server/http_server.hpp"
#include "basalt/server/mcp_server.hpp"
#include "basalt/platform/platform_runner.hpp"

#include "basalt/common/config.hpp"

#include <charconv>
#include <iostream>
#include <string_view>
#include <utility>

namespace {

bool parse_port(const std::string_view text, int& port) {
    unsigned int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < 1 || value > 65535) {
        return false;
    }
    port = static_cast<int>(value);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    bool mcp_stdio = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--mcp-stdio") mcp_stdio = true;
    }
    if (mcp_stdio) {
        if (argc != 2) {
            std::cerr << "--mcp-stdio cannot be combined with HTTP server options\n";
            return 2;
        }
        const auto data_dir = basalt::server::McpServer::resolve_data_dir();
        if (data_dir.empty()) {
            std::cerr << "BASALT MCP private data storage could not be secured; refusing to start\n";
            return 1;
        }
        basalt::server::McpServer server(data_dir / "mcp_audit.jsonl",
                                         data_dir / "mcp_killswitch.blog");
        return server.run_stdio(std::cin, std::cout) ? 0 : 1;
    }
    auto loaded_config = basalt::common::ConfigLoader::from_environment();
    if (!loaded_config) {
        std::cerr << "Invalid configuration: " << loaded_config.error().message << '\n';
        return 2;
    }
    basalt::common::AppConfig config = std::move(loaded_config).value();
    int port = static_cast<int>(config.port);
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--help" || argument == "-h") {
            std::cout << "Usage: basalt_server [--port 1-65535] | basalt_server --mcp-stdio\n";
            return 0;
        }
        if (argument == "--port" && i + 1 < argc) {
            if (!parse_port(argv[++i], port)) {
                std::cerr << "Invalid port number\n";
                return 2;
            }
        } else {
            std::cerr << "Unknown or incomplete argument: " << argument << '\n';
            return 2;
        }
    }

    config.port = static_cast<std::uint32_t>(port);
    basalt::platform::PlatformRunner platform;
    const auto session = platform.run(12);
    if (!session) {
        std::cerr << "Failed to initialize the synthetic paper session: "
                  << session.error().code << ": " << session.error().message << '\n';
        return 1;
    }
    basalt::server::HttpServer server(config, &platform);
    std::cout << "BASALT serving http://" << config.bind_address << ':' << port << "\n";
    std::cout << "Synthetic paper session: " << session.value().ticks_processed
              << " ticks, " << session.value().orders << " orders, "
              << session.value().fills << " fills, " << session.value().risk_rejections
              << " risk rejections; portfolio at /api/v1/portfolio/1\n";
    if (!server.run()) {
        std::cerr << "Failed to start HTTP server on port " << port << '\n';
        return 1;
    }
    return 0;
}
