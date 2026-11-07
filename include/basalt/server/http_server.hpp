#pragma once

#include "basalt/bench/benchmark_runner.hpp"
#include "basalt/common/config.hpp"
#include "basalt/server/simulation_facade.hpp"

#include <httplib.h>

#include <cstdint>
#include <mutex>
#include <random>
#include <string>

namespace basalt::server {

class HttpServer {
public:
    explicit HttpServer(int port = 8080);
    explicit HttpServer(const common::AppConfig& config);
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    [[nodiscard]] bool run();
    // Test/embedding support: bind the configured address to an OS-assigned free
    // port (updating port() and the Host/Origin allowlist), then serve it with
    // run_bound(). Must be called before run_bound() starts on another thread.
    [[nodiscard]] bool bind_to_any_port();
    [[nodiscard]] bool run_bound();
    void wait_until_ready() const;
    void stop() noexcept;
    [[nodiscard]] int port() const noexcept { return port_; }

private:
    int port_;
    std::string bind_address_{"127.0.0.1"};
    httplib::Server server_{};
    SimulationFacade simulation_{};
    bench::BenchmarkRunner benchmark_runner_{};
    mutable std::mutex simulation_rng_mutex_{};
    std::mt19937_64 simulation_rng_{42};

    void register_routes();
    bool guard_request(const httplib::Request& request, httplib::Response& response) const;
    [[nodiscard]] static std::string ui_html();
};

} // namespace basalt::server
