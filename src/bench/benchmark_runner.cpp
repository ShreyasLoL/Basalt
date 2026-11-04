#include "atlas/bench/benchmark_runner.hpp"

#include "atlas/core/order_book.hpp"
#include "atlas/core/vector_order_book.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace atlas::bench {
namespace {

using Clock = std::chrono::steady_clock;

struct RunSummary {
    BenchmarkResult result{};
    BookSnapshot final_snapshot{};
};

std::uint64_t percentile(const std::vector<std::uint64_t>& sorted,
                         const double probability) {
    if (sorted.empty()) return 0;
    const std::size_t rank = static_cast<std::size_t>(
        std::ceil(probability * static_cast<double>(sorted.size())));
    return sorted[std::max<std::size_t>(1, rank) - 1];
}

BenchmarkResult summarize(const std::string& name, const std::string& implementation,
                          const std::vector<std::uint64_t>& latencies,
                          const std::chrono::nanoseconds elapsed,
                          const std::uint64_t trade_count,
                          const std::uint64_t cancel_count) {
    BenchmarkResult result{};
    result.profile_name = name;
    result.implementation = implementation;
    result.orders_processed = static_cast<std::uint64_t>(latencies.size());
    result.trades_executed = trade_count;
    result.cancellations_executed = cancel_count;
    result.total_time_ms = std::chrono::duration<double, std::milli>(elapsed).count();
    const double elapsed_seconds = std::chrono::duration<double>(elapsed).count();
    result.throughput_ops_per_sec = elapsed_seconds > 0
        ? static_cast<double>(result.orders_processed) / elapsed_seconds : 0;

    std::vector<std::uint64_t> sorted_latencies = latencies;
    std::sort(sorted_latencies.begin(), sorted_latencies.end());
    if (!sorted_latencies.empty()) {
        const std::uint64_t sum = std::accumulate(sorted_latencies.begin(), sorted_latencies.end(),
                                                   std::uint64_t{0});
        result.avg_latency_ns = static_cast<double>(sum) /
                                static_cast<double>(sorted_latencies.size());
    }
    result.p50_latency_ns = percentile(sorted_latencies, 0.50);
    result.p95_latency_ns = percentile(sorted_latencies, 0.95);
    result.p99_latency_ns = percentile(sorted_latencies, 0.99);
    result.p999_latency_ns = percentile(sorted_latencies, 0.999);
    return result;
}

void warmup(const std::vector<WorkloadCommand>& commands) {
    OrderBook map_book;
    VectorOrderBook vector_book;
    SequenceNumber map_sequence = 0;
    SequenceNumber vector_sequence = 0;
    TradeId map_trade_id = 0;
    TradeId vector_trade_id = 0;
    for (const WorkloadCommand& command : commands) {
        if (command.action == ActionType::Submit) {
            const auto make_order = [&](const SequenceNumber sequence) {
                Order order{};
                order.id = command.order.id;
                order.side = command.order.side;
                order.type = command.order.type;
                order.price = command.order.type == OrderType::Market ? 0 : command.order.price;
                order.initial_quantity = command.order.quantity;
                order.remaining_quantity = command.order.quantity;
                order.sequence = sequence;
                order.timestamp_ns = sequence;
                return order;
            };

            Order map_incoming = make_order(++map_sequence);
            std::vector<Trade> map_trades;
            map_book.match_against_book(map_incoming, map_sequence, map_trade_id,
                                        map_incoming.timestamp_ns, map_trades);
            if (map_incoming.remaining_quantity > 0 && map_incoming.type == OrderType::Limit) {
                (void)map_book.insert_resting_order(map_incoming);
            }

            Order vector_incoming = make_order(++vector_sequence);
            std::vector<Trade> vector_trades;
            (void)vector_book.submit_order(vector_incoming, vector_sequence, vector_trade_id,
                                           vector_trades);
        } else {
            if (map_book.cancel_order(command.cancel_order_id).success) ++map_sequence;
            if (vector_book.cancel_order(command.cancel_order_id)) ++vector_sequence;
        }
    }
}

RunSummary run_map_book(const WorkloadProfile profile,
                        const std::vector<WorkloadCommand>& commands) {
    OrderBook book;
    SequenceNumber sequence = 0;
    TradeId trade_id = 0;
    std::vector<std::uint64_t> latencies(commands.size(), 0);
    std::uint64_t trade_count = 0;
    std::uint64_t cancel_count = 0;
    const auto batch_start = Clock::now();
    for (std::size_t i = 0; i < commands.size(); ++i) {
        const auto start = Clock::now();
        const WorkloadCommand& command = commands[i];
        if (command.action == ActionType::Submit) {
            Order incoming{};
            incoming.id = command.order.id;
            incoming.side = command.order.side;
            incoming.type = command.order.type;
            incoming.price = command.order.type == OrderType::Market ? 0 : command.order.price;
            incoming.initial_quantity = command.order.quantity;
            incoming.remaining_quantity = command.order.quantity;
            incoming.sequence = ++sequence;
            incoming.timestamp_ns = incoming.sequence;
            std::vector<Trade> trades;
            book.match_against_book(incoming, sequence, trade_id, incoming.timestamp_ns, trades);
            trade_count += trades.size();
            if (incoming.remaining_quantity > 0 && incoming.type == OrderType::Limit) {
                (void)book.insert_resting_order(incoming);
            }
        } else {
            const CancelResult result = book.cancel_order(command.cancel_order_id);
            if (result.success) {
                ++sequence;
                ++cancel_count;
            }
        }
        const auto stop = Clock::now();
        latencies[i] = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count());
    }
    const auto batch_end = Clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(batch_end - batch_start);

    std::string error;
    if (!book.verify_invariants(&error)) {
        throw std::runtime_error("MapListOrderBook invariant failure: " + error);
    }
    RunSummary summary{};
    summary.result = summarize(std::string(profile_name(profile)),
                              "MapListOrderBook (Primary)", latencies, elapsed,
                              trade_count, cancel_count);
    summary.final_snapshot = book.get_snapshot(std::numeric_limits<std::size_t>::max());
    return summary;
}

RunSummary run_vector_book(const WorkloadProfile profile,
                           const std::vector<WorkloadCommand>& commands) {
    VectorOrderBook book;
    SequenceNumber sequence = 0;
    TradeId trade_id = 0;
    std::vector<std::uint64_t> latencies(commands.size(), 0);
    std::uint64_t trade_count = 0;
    std::uint64_t cancel_count = 0;
    const auto batch_start = Clock::now();
    for (std::size_t i = 0; i < commands.size(); ++i) {
        const auto start = Clock::now();
        const WorkloadCommand& command = commands[i];
        if (command.action == ActionType::Submit) {
            Order incoming{};
            incoming.id = command.order.id;
            incoming.side = command.order.side;
            incoming.type = command.order.type;
            incoming.price = command.order.type == OrderType::Market ? 0 : command.order.price;
            incoming.initial_quantity = command.order.quantity;
            incoming.remaining_quantity = command.order.quantity;
            incoming.sequence = ++sequence;
            incoming.timestamp_ns = incoming.sequence;
            std::vector<Trade> trades;
            (void)book.submit_order(incoming, sequence, trade_id, trades);
            trade_count += trades.size();
        } else if (book.cancel_order(command.cancel_order_id)) {
            ++sequence;
            ++cancel_count;
        }
        const auto stop = Clock::now();
        latencies[i] = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count());
    }
    const auto batch_end = Clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(batch_end - batch_start);

    std::string error;
    if (!book.verify_invariants(&error)) {
        throw std::runtime_error("VectorOrderBook invariant failure: " + error);
    }
    RunSummary summary{};
    summary.result = summarize(std::string(profile_name(profile)),
                              "VectorOrderBook (Baseline)", latencies, elapsed,
                              trade_count, cancel_count);
    summary.final_snapshot = book.get_snapshot(std::numeric_limits<std::size_t>::max());
    return summary;
}

std::vector<WorkloadCommand> warmup_workload(const WorkloadProfile profile) {
    return WorkloadGenerator::generate(profile, 10000, 2026);
}

} // namespace

std::vector<BenchmarkResult> BenchmarkRunner::run(const WorkloadProfile profile,
                                                  const std::size_t order_count,
                                                  const bool compare_baseline) const {
    if (order_count == 0) throw std::invalid_argument("Benchmark order count must be positive");
    const auto commands = WorkloadGenerator::generate(profile, order_count, 42);
    const auto warmup_commands = warmup_workload(profile);
    warmup(warmup_commands);

    RunSummary primary = run_map_book(profile, commands);
    std::vector<BenchmarkResult> results;
    results.push_back(primary.result);
    if (!compare_baseline) return results;

    RunSummary baseline = run_vector_book(profile, commands);
    if (primary.result.trades_executed != baseline.result.trades_executed ||
        primary.result.cancellations_executed != baseline.result.cancellations_executed ||
        primary.final_snapshot != baseline.final_snapshot) {
        throw std::runtime_error("MapListOrderBook and VectorOrderBook diverged for workload " +
                                 std::string(profile_id(profile)));
    }
    results.push_back(std::move(baseline.result));
    return results;
}

std::vector<BenchmarkResult> BenchmarkRunner::run_all(const std::size_t order_count,
                                                      const bool compare_baseline) const {
    std::vector<BenchmarkResult> results;
    results.reserve(WORKLOAD_PROFILES.size() * (compare_baseline ? 2U : 1U));
    for (const auto& profile : WORKLOAD_PROFILES) {
        auto profile_results = run(profile.profile, order_count, compare_baseline);
        results.insert(results.end(), profile_results.begin(), profile_results.end());
    }
    return results;
}

std::string format_ascii_table(const std::vector<BenchmarkResult>& results) {
    std::ostringstream output;
    output << std::left << std::setw(27) << "Workload"
           << std::setw(31) << "Implementation"
           << std::right << std::setw(13) << "Ops/s"
           << std::setw(11) << "Avg ns"
           << std::setw(10) << "p50 ns"
           << std::setw(10) << "p95 ns"
           << std::setw(10) << "p99 ns"
           << std::setw(11) << "p99.9 ns" << '\n';
    output << std::string(123, '-') << '\n';
    for (const BenchmarkResult& result : results) {
        output << std::left << std::setw(27) << result.profile_name
               << std::setw(31) << result.implementation
               << std::right << std::fixed << std::setprecision(0)
               << std::setw(13) << result.throughput_ops_per_sec
               << std::setw(11) << result.avg_latency_ns
               << std::setw(10) << result.p50_latency_ns
               << std::setw(10) << result.p95_latency_ns
               << std::setw(10) << result.p99_latency_ns
               << std::setw(11) << result.p999_latency_ns << '\n';
    }
    return output.str();
}

} // namespace atlas::bench
