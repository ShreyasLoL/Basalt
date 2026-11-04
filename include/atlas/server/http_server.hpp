#pragma once

#include "atlas/bench/benchmark_runner.hpp"
#include "atlas/core/matching_engine.hpp"

#include <httplib.h>

#include <cstdint>
#include <mutex>
#include <random>

namespace atlas::server {

class HttpServer {
public:
    explicit HttpServer(int port = 8080);
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    [[nodiscard]] bool run();
    void stop() noexcept;
    [[nodiscard]] int port() const noexcept { return port_; }

private:
    int port_;
    httplib::Server server_{};
    MatchingEngine engine_{};
    bench::BenchmarkRunner benchmark_runner_{};
    mutable std::mutex engine_mutex_{};
    std::mt19937_64 simulation_rng_{42};
    OrderId next_order_id_{10000};

    void register_routes();
    [[nodiscard]] static std::string ui_html();
};

} // namespace atlas::server
