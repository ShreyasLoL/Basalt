#include "basalt/bench/benchmark_runner.hpp"

#include "basalt/core/arena_order_book.hpp"
#include "basalt/core/order_book.hpp"
#include "basalt/core/vector_order_book.hpp"
#include "basalt/market_data/event_bus.hpp"
#include "basalt/market_data/pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <numeric>
#include <map>
#include <sstream>
#include <stdexcept>

namespace basalt::bench {
namespace {

using Clock = std::chrono::steady_clock;

struct RunSummary {
    BenchmarkResult result{};
    std::vector<Trade> trade_log{};
    std::map<common::InstrumentId, BookSnapshot> final_snapshots{};
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
    result.p90_latency_ns = percentile(sorted_latencies, 0.90);
    result.p95_latency_ns = percentile(sorted_latencies, 0.95);
    result.p99_latency_ns = percentile(sorted_latencies, 0.99);
    result.p999_latency_ns = percentile(sorted_latencies, 0.999);
    return result;
}

void warmup(const std::vector<WorkloadCommand>& commands) {
    std::map<common::InstrumentId, OrderBook> map_books;
    std::map<common::InstrumentId, VectorOrderBook> vector_books;
    SequenceNumber map_sequence = 0;
    SequenceNumber vector_sequence = 0;
    TradeId map_trade_id = 0;
    TradeId vector_trade_id = 0;
    for (const WorkloadCommand& command : commands) {
        if (command.action == ActionType::Submit) {
            auto& map_book = map_books[command.instrument_id];
            auto& vector_book = vector_books[command.instrument_id];
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
            if (map_books[command.instrument_id].cancel_order(command.cancel_order_id).success) ++map_sequence;
            if (vector_books[command.instrument_id].cancel_order(command.cancel_order_id)) ++vector_sequence;
        }
    }
}

RunSummary run_map_book(const WorkloadProfile profile,
                        const std::vector<WorkloadCommand>& commands) {
    std::map<common::InstrumentId, OrderBook> books;
    SequenceNumber sequence = 0;
    TradeId trade_id = 0;
    std::vector<std::uint64_t> latencies(commands.size(), 0);
    std::uint64_t trade_count = 0;
    std::uint64_t cancel_count = 0;
    std::vector<Trade> trade_log;
    const auto batch_start = Clock::now();
    for (std::size_t i = 0; i < commands.size(); ++i) {
        const auto start = Clock::now();
        const WorkloadCommand& command = commands[i];
        auto& book = books[command.instrument_id];
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
            trade_log.insert(trade_log.end(), trades.begin(), trades.end());
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

    RunSummary summary{};
    for (const auto& [instrument, book] : books) {
        std::string error;
        if (!book.verify_invariants(&error)) {
            throw std::runtime_error("MapListOrderBook invariant failure: " + error);
        }
        summary.final_snapshots.emplace(instrument,
            book.get_snapshot(std::numeric_limits<std::size_t>::max()));
    }
    summary.result = summarize(std::string(profile_name(profile)),
                              "StandardOrderBook (map/list)", latencies, elapsed,
                              trade_count, cancel_count);
    summary.trade_log = std::move(trade_log);
    return summary;
}

RunSummary run_vector_book(const WorkloadProfile profile,
                           const std::vector<WorkloadCommand>& commands) {
    std::map<common::InstrumentId, VectorOrderBook> books;
    SequenceNumber sequence = 0;
    TradeId trade_id = 0;
    std::vector<std::uint64_t> latencies(commands.size(), 0);
    std::uint64_t trade_count = 0;
    std::uint64_t cancel_count = 0;
    std::vector<Trade> trade_log;
    const auto batch_start = Clock::now();
    for (std::size_t i = 0; i < commands.size(); ++i) {
        const auto start = Clock::now();
        const WorkloadCommand& command = commands[i];
        auto& book = books[command.instrument_id];
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
            trade_log.insert(trade_log.end(), trades.begin(), trades.end());
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

    RunSummary summary{};
    for (const auto& [instrument, book] : books) {
        std::string error;
        if (!book.verify_invariants(&error)) {
            throw std::runtime_error("VectorOrderBook invariant failure: " + error);
        }
        summary.final_snapshots.emplace(instrument,
            book.get_snapshot(std::numeric_limits<std::size_t>::max()));
    }
    summary.result = summarize(std::string(profile_name(profile)),
                              "BaselineOrderBook (vector)", latencies, elapsed,
                              trade_count, cancel_count);
    summary.trade_log = std::move(trade_log);
    return summary;
}

RunSummary run_arena_book(const WorkloadProfile profile,
                          const std::vector<WorkloadCommand>& commands) {
    std::map<common::InstrumentId, std::size_t> capacity_by_instrument;
    for (const auto& command : commands) ++capacity_by_instrument[command.instrument_id];
    std::map<common::InstrumentId, ArenaOrderBook> books;
    for (const auto& [instrument, capacity] : capacity_by_instrument)
        books.try_emplace(instrument, capacity);
    SequenceNumber sequence = 0;
    TradeId trade_id = 0;
    std::vector<std::uint64_t> latencies(commands.size(), 0);
    std::uint64_t trade_count = 0;
    std::uint64_t cancel_count = 0;
    std::uint64_t pool_exhaustions = 0;
    std::vector<Trade> trade_log;
    const auto batch_start = Clock::now();
    for (std::size_t i = 0; i < commands.size(); ++i) {
        const auto start = Clock::now();
        const WorkloadCommand& command = commands[i];
        auto& book = books.at(command.instrument_id);
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
            if (book.last_error() == "POOL_EXHAUSTED") ++pool_exhaustions;
            trade_count += trades.size();
            trade_log.insert(trade_log.end(), trades.begin(), trades.end());
        } else if (book.cancel_order(command.cancel_order_id).success) {
            ++sequence;
            ++cancel_count;
        }
        latencies[i] = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - batch_start);
    RunSummary summary{};
    for (const auto& [instrument, book] : books) {
        std::string error;
        if (!book.verify_invariants(&error)) throw std::runtime_error("ArenaOrderBook invariant failure: " + error);
        summary.final_snapshots.emplace(instrument,
            book.get_snapshot(std::numeric_limits<std::size_t>::max()));
    }
    summary.result = summarize(std::string(profile_name(profile)),
        "ArenaOrderBook (pooled nodes)", latencies, elapsed, trade_count, cancel_count);
    summary.trade_log = std::move(trade_log);
    summary.result.pool_exhaustions = pool_exhaustions;
    for (const auto& [instrument, book] : books) {
        (void)instrument;
        summary.result.pool_allocations += book.pool_allocations();
        summary.result.pool_peak_active += book.pool_peak_active();
        summary.result.pool_capacity += book.pool_capacity();
    }
    return summary;
}

RunSummary run_event_bus(const WorkloadProfile profile, const std::size_t count) {
    const bool slow_consumer = profile == WorkloadProfile::SlowConsumerBackpressure;
    const std::size_t capacity = slow_consumer ? std::min<std::size_t>(64, count) : count;
    market_data::BoundedMarketEventBus bus(std::max<std::size_t>(1, capacity), 1);
    market_data::InstrumentMaster instruments;
    for (common::InstrumentId id = 1; id <= 50; ++id) {
        market_data::InstrumentDefinition instrument{};
        instrument.id = id;
        instrument.exchange = market_data::Exchange::Sim;
        instrument.segment = "EQ";
        instrument.symbol = "BENCH" + std::to_string(id);
        instrument.display_symbol = "BENCH:" + std::to_string(id);
        instrument.tick_size_micro = 500;
        instrument.lot_size = 1;
        instrument.quantity_step = 1;
        instrument.provider_mappings.push_back({"BENCH", "B/" + std::to_string(id), 0, std::nullopt});
        const auto added = instruments.add(std::move(instrument));
        if (!added) throw std::runtime_error("Market-data benchmark instrument setup failed: " + added.error().message);
    }
    market_data::MarketDataPipeline pipeline(instruments);
    pipeline.register_provider("BENCH", {common::DataSourceType::Synthetic, 10'000'000, 10'000'000});
    pipeline.set_event_sinks([&bus](market_data::CanonicalMarketEvent event) {
        return bus.publish_durable(std::move(event), std::chrono::milliseconds(0));
    }, {});
    std::vector<std::uint64_t> latencies(count, 0);
    std::uint64_t accepted = 0;
    const auto batch_start = Clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        const auto start = Clock::now();
        const auto instrument_id = static_cast<common::InstrumentId>(i % 50) + 1;
        market_data::RawMarketEvent event{};
        event.provider = "BENCH";
        event.event_id = "bench-" + std::to_string(i);
        event.provider_instrument_key = "B/" + std::to_string(instrument_id);
        event.kind = market_data::MarketEventKind::Trade;
        event.event_time_ns = static_cast<std::int64_t>(1'000'000'000 + i * 1'000);
        event.provider_sequence = i / 50 + 1;
        event.price = "100.0000";
        event.quantity = "1";
        const auto processed = pipeline.process(event, static_cast<std::int64_t>(i + 1), event.event_time_ns);
        if (processed) ++accepted;
        latencies[i] = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - batch_start);
    const auto stats = bus.stats();
    const auto dropped = static_cast<std::uint64_t>(count) - accepted;
    while (bus.pop_durable(std::chrono::milliseconds(0))) {}
    RunSummary summary{};
    summary.result = summarize(std::string(profile_name(profile)), "BoundedMarketEventBus",
        latencies, elapsed, 0, 0);
    summary.result.events_accepted = accepted;
    summary.result.events_dropped = dropped;
    summary.result.queue_high_water = stats.durable_depth;
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
    if (profile == WorkloadProfile::MarketDataBurst ||
        profile == WorkloadProfile::SlowConsumerBackpressure)
        return {run_event_bus(profile, order_count).result};
    const auto commands = WorkloadGenerator::generate(profile, order_count, 42);
    const auto warmup_commands = warmup_workload(profile);
    warmup(warmup_commands);

    RunSummary primary = run_map_book(profile, commands);
    std::vector<BenchmarkResult> results;
    results.push_back(primary.result);
    RunSummary arena = run_arena_book(profile, commands);
    if (primary.result.trades_executed != arena.result.trades_executed ||
        primary.result.cancellations_executed != arena.result.cancellations_executed ||
        primary.trade_log != arena.trade_log || primary.final_snapshots != arena.final_snapshots ||
        arena.result.pool_exhaustions != 0) {
        throw std::runtime_error("Standard and pooled arena books diverged for workload " +
                                 std::string(profile_id(profile)));
    }
    if (compare_baseline) {
        RunSummary baseline = run_vector_book(profile, commands);
        if (primary.result.trades_executed != baseline.result.trades_executed ||
            primary.result.cancellations_executed != baseline.result.cancellations_executed ||
            primary.trade_log != baseline.trade_log || primary.final_snapshots != baseline.final_snapshots)
            throw std::runtime_error("Standard and baseline vector books diverged for workload " +
                                     std::string(profile_id(profile)));
        results.push_back(std::move(baseline.result));
    }
    results.push_back(std::move(arena.result));
    return results;
}

std::vector<BenchmarkResult> BenchmarkRunner::run_all(const std::size_t order_count,
                                                      const bool compare_baseline) const {
    std::vector<BenchmarkResult> results;
    results.reserve(WORKLOAD_PROFILES.size() * (compare_baseline ? 3U : 2U));
    for (const auto& profile : WORKLOAD_PROFILES) {
        auto profile_results = run(profile.profile, order_count, compare_baseline);
        results.insert(results.end(), profile_results.begin(), profile_results.end());
    }
    return results;
}

std::string format_ascii_table(const std::vector<BenchmarkResult>& results) {
    std::ostringstream output;
    output << std::left << std::setw(31) << "Workload"
           << std::setw(31) << "Implementation"
           << std::right << std::setw(12) << "Ops/s"
           << std::setw(10) << "Avg ns"
           << std::setw(10) << "p50 ns"
           << std::setw(10) << "p90 ns"
           << std::setw(10) << "p95 ns"
           << std::setw(10) << "p99 ns"
           << std::setw(11) << "p99.9 ns"
           << std::setw(11) << "Accepted"
           << std::setw(10) << "Dropped"
           << std::setw(8) << "Q high"
           << std::setw(12) << "Pool alloc"
           << std::setw(11) << "Pool peak"
           << std::setw(11) << "Pool cap"
           << std::setw(12) << "Pool errors" << '\n';
    output << std::string(210, '-') << '\n';
    for (const BenchmarkResult& result : results) {
        output << std::left << std::setw(31) << result.profile_name
               << std::setw(31) << result.implementation
               << std::right << std::fixed << std::setprecision(0)
               << std::setw(12) << result.throughput_ops_per_sec
               << std::setw(10) << result.avg_latency_ns
               << std::setw(10) << result.p50_latency_ns
               << std::setw(10) << result.p90_latency_ns
               << std::setw(10) << result.p95_latency_ns
               << std::setw(10) << result.p99_latency_ns
               << std::setw(11) << result.p999_latency_ns
               << std::setw(11) << result.events_accepted
               << std::setw(10) << result.events_dropped
               << std::setw(8) << result.queue_high_water
               << std::setw(12) << result.pool_allocations
               << std::setw(11) << result.pool_peak_active
               << std::setw(11) << result.pool_capacity
               << std::setw(12) << result.pool_exhaustions << '\n';
    }
    return output.str();
}

} // namespace basalt::bench
