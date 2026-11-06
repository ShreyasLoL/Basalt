#pragma once

#include "basalt/bench/workload_generator.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace basalt::bench {

struct BenchmarkResult {
    std::string profile_name{};
    std::string implementation{};
    std::uint64_t orders_processed{0};
    std::uint64_t trades_executed{0};
    std::uint64_t cancellations_executed{0};
    std::uint64_t events_accepted{0};
    std::uint64_t events_dropped{0};
    std::uint64_t queue_high_water{0};
    std::uint64_t pool_exhaustions{0};
    std::uint64_t pool_allocations{0};
    std::uint64_t pool_peak_active{0};
    std::uint64_t pool_capacity{0};
    double total_time_ms{0};
    double throughput_ops_per_sec{0};
    double avg_latency_ns{0};
    std::uint64_t p50_latency_ns{0};
    std::uint64_t p90_latency_ns{0};
    std::uint64_t p95_latency_ns{0};
    std::uint64_t p99_latency_ns{0};
    std::uint64_t p999_latency_ns{0};
};

class BenchmarkRunner {
public:
    [[nodiscard]] std::vector<BenchmarkResult> run(
        WorkloadProfile profile, std::size_t order_count,
        bool compare_baseline = true) const;
    [[nodiscard]] std::vector<BenchmarkResult> run_all(
        std::size_t order_count, bool compare_baseline = true) const;
};

[[nodiscard]] std::string format_ascii_table(const std::vector<BenchmarkResult>& results);

} // namespace basalt::bench
