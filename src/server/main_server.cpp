#include "basalt/server/http_server.hpp"
#include "basalt/server/mcp_server.hpp"

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
        basalt::server::McpServer server;
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
    basalt::server::HttpServer server(config);
    std::cout << "BASALT serving http://" << config.bind_address << ':' << port << "\n";
    if (!server.run()) {
        std::cerr << "Failed to start HTTP server on port " << port << '\n';
        return 1;
    }
    return 0;
}
