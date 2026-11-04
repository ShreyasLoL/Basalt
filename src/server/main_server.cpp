#include "atlas/server/http_server.hpp"

#include <charconv>
#include <iostream>
#include <string_view>

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
    int port = 8080;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--help" || argument == "-h") {
            std::cout << "Usage: atlas_server [--port 1-65535]\n";
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

    atlas::server::HttpServer server(port);
    std::cout << "Project Atlas serving http://127.0.0.1:" << port << "\n";
    if (!server.run()) {
        std::cerr << "Failed to start HTTP server on port " << port << '\n';
        return 1;
    }
    return 0;
}
