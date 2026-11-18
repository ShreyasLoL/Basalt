#include "basalt/bench/benchmark_runner.hpp"
#include "basalt/bench/telemetry.hpp"
#include "basalt/core/arena_order_book.hpp"
#include "basalt/core/order_book.hpp"
#include "basalt/core/object_pool.hpp"
#include "basalt/core/vector_order_book.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <map>
#include <unordered_map>

namespace basalt::bench {

namespace {

void assert_randomized_book_parity(const WorkloadProfile profile, const std::uint64_t seed) {
    const auto commands = WorkloadGenerator::generate(profile, 750, seed);
    OrderBook map_book;
    VectorOrderBook vector_book;
    ArenaOrderBook arena_book(commands.size() + 1U);
    SequenceNumber map_sequence = 0;
    SequenceNumber vector_sequence = 0;
    SequenceNumber arena_sequence = 0;
    TradeId map_trade_id = 0;
    TradeId vector_trade_id = 0;
    TradeId arena_trade_id = 0;
    std::map<OrderId, Quantity> initial_quantity;
    std::map<OrderId, Quantity> traded_quantity;
    std::map<OrderId, Quantity> cancelled_quantity;

    const auto invariants_hold = [&] {
        std::string map_error;
        std::string vector_error;
        std::string arena_error;
        EXPECT_TRUE(map_book.verify_invariants(&map_error)) << map_error;
        EXPECT_TRUE(vector_book.verify_invariants(&vector_error)) << vector_error;
        EXPECT_TRUE(arena_book.verify_invariants(&arena_error)) << arena_error;
        EXPECT_EQ(map_book.get_snapshot(std::numeric_limits<std::size_t>::max()),
                  vector_book.get_snapshot(std::numeric_limits<std::size_t>::max()));
        EXPECT_EQ(map_book.get_snapshot(std::numeric_limits<std::size_t>::max()),
                  arena_book.get_snapshot(std::numeric_limits<std::size_t>::max()));
    };

    for (const auto& command : commands) {
        if (command.action == ActionType::Cancel) {
            const auto cancelled = map_book.cancel_order(command.cancel_order_id);
            EXPECT_EQ(vector_book.cancel_order(command.cancel_order_id), cancelled.success);
            EXPECT_EQ(arena_book.cancel_order(command.cancel_order_id).success, cancelled.success);
            if (cancelled.success) {
                cancelled_quantity[command.cancel_order_id] += cancelled.cancelled_quantity;
                ++map_sequence;
                ++vector_sequence;
                ++arena_sequence;
            }
            invariants_hold();
            continue;
        }

        const auto& request = command.order;
        initial_quantity[request.id] = request.quantity;
        auto make_order = [&](const SequenceNumber sequence) {
            Order order{};
            order.id = request.id;
            order.side = request.side;
            order.type = request.type;
            order.price = request.type == OrderType::Market ? 0 : request.price;
            order.initial_quantity = request.quantity;
            order.remaining_quantity = request.quantity;
            order.sequence = sequence;
            order.timestamp_ns = static_cast<TimestampNs>(sequence);
            return order;
        };

        const auto before = map_book.get_snapshot(std::numeric_limits<std::size_t>::max());
        std::unordered_map<OrderId, Order> before_by_id;
        before_by_id.reserve(before.resting_orders.size());
        for (const auto& order : before.resting_orders) before_by_id.emplace(order.id, order);

        Order map_incoming = make_order(++map_sequence);
        std::vector<Trade> map_trades;
        map_book.match_against_book(map_incoming, map_sequence, map_trade_id,
                                    map_incoming.timestamp_ns, map_trades);
        OrderStatus map_status{};
        if (map_incoming.remaining_quantity == 0) map_status = OrderStatus::Filled;
        else if (map_incoming.type == OrderType::Market) {
            map_status = map_incoming.remaining_quantity < map_incoming.initial_quantity
                ? OrderStatus::PartiallyFilled : OrderStatus::Cancelled;
        } else if (map_book.insert_resting_order(map_incoming)) {
            map_status = map_incoming.remaining_quantity < map_incoming.initial_quantity
                ? OrderStatus::PartiallyFilled : OrderStatus::New;
        } else map_status = OrderStatus::Rejected;

        Order vector_incoming = make_order(++vector_sequence);
        std::vector<Trade> vector_trades;
        const auto vector_status = vector_book.submit_order(vector_incoming, vector_sequence,
                                                            vector_trade_id, vector_trades);
        Order arena_incoming = make_order(++arena_sequence);
        std::vector<Trade> arena_trades;
        arena_book.match_against_book(arena_incoming, arena_sequence, arena_trade_id,
                                      arena_incoming.timestamp_ns, arena_trades);
        const auto arena_status = arena_incoming.remaining_quantity == 0 ? OrderStatus::Filled :
            (arena_incoming.type == OrderType::Market
                ? (arena_incoming.remaining_quantity < arena_incoming.initial_quantity
                    ? OrderStatus::PartiallyFilled : OrderStatus::Cancelled)
                : (arena_book.has_order(arena_incoming.id)
                    ? (arena_incoming.remaining_quantity < arena_incoming.initial_quantity
                        ? OrderStatus::PartiallyFilled : OrderStatus::New)
                    : OrderStatus::Rejected));

        EXPECT_EQ(vector_status, map_status);
        EXPECT_EQ(arena_status, map_status);
        EXPECT_EQ(vector_trades, map_trades);
        EXPECT_EQ(arena_trades, map_trades);
        Price previous_trade_price = 0;
        std::map<Price, SequenceNumber> previous_fifo_sequence;
        for (std::size_t i = 0; i < map_trades.size(); ++i) {
            const auto& trade = map_trades[i];
            EXPECT_GT(trade.quantity, 0U);
            const OrderId resting_id = request.side == Side::Buy ? trade.sell_order_id : trade.buy_order_id;
            const auto resting = before_by_id.find(resting_id);
            ASSERT_NE(resting, before_by_id.end());
            EXPECT_EQ(trade.price, resting->second.price);
            if (request.type == OrderType::Limit) {
                if (request.side == Side::Buy) EXPECT_LE(trade.price, request.price);
                else EXPECT_GE(trade.price, request.price);
            }
            if (i > 0) {
                if (request.side == Side::Buy) EXPECT_GE(trade.price, previous_trade_price);
                else EXPECT_LE(trade.price, previous_trade_price);
                EXPECT_GT(trade.sequence, map_trades[i - 1U].sequence);
            }
            previous_trade_price = trade.price;
            const auto prior_fifo = previous_fifo_sequence.find(trade.price);
            if (prior_fifo != previous_fifo_sequence.end())
                EXPECT_LT(prior_fifo->second, resting->second.sequence);
            previous_fifo_sequence[trade.price] = resting->second.sequence;
            traded_quantity[trade.buy_order_id] += trade.quantity;
            traded_quantity[trade.sell_order_id] += trade.quantity;
        }
        if (request.type == OrderType::Market)
            cancelled_quantity[request.id] += map_incoming.remaining_quantity;
        invariants_hold();
    }

    for (const auto& [id, quantity] : initial_quantity) {
        const Order* resting = map_book.get_order(id);
        const Quantity remaining = resting == nullptr ? 0 : resting->remaining_quantity;
        EXPECT_EQ(quantity, traded_quantity[id] + cancelled_quantity[id] + remaining)
            << "quantity conservation failed for order " << id;
    }
}

} // namespace

TEST(WorkloadProfileTest, ExposesTwelveStableProfilesAndReproducibleInstrumentDistribution) {
    EXPECT_EQ(WORKLOAD_PROFILES.size(), 12U);
    for (const auto& info : WORKLOAD_PROFILES) {
        WorkloadProfile parsed{};
        EXPECT_TRUE(parse_profile(info.id, parsed));
        EXPECT_EQ(parsed, info.profile);
        EXPECT_FALSE(profile_name(parsed).empty());
    }
    const auto first = WorkloadGenerator::generate(WorkloadProfile::MultiInstrument50, 500, 2026);
    const auto second = WorkloadGenerator::generate(WorkloadProfile::MultiInstrument50, 500, 2026);
    ASSERT_EQ(first.size(), 500U);
    ASSERT_EQ(second.size(), first.size());
    std::array<std::size_t, 50> counts{};
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_EQ(first[i].order.id, second[i].order.id);
        EXPECT_EQ(first[i].order.side, second[i].order.side);
        EXPECT_EQ(first[i].order.price, second[i].order.price);
        EXPECT_EQ(first[i].instrument_id, second[i].instrument_id);
        ++counts[first[i].instrument_id - 1];
    }
    EXPECT_TRUE(std::all_of(counts.begin(), counts.end(), [](const auto count) { return count == 10; }));
    EXPECT_TRUE(WorkloadGenerator::generate(WorkloadProfile::MarketDataBurst, 10).empty());
}

TEST(ObjectPoolTest, ExhaustionReclamationAndGenerationsAreDeterministic) {
    ObjectPool<Order> pool(1);
    Order first{};
    first.id = 11;
    const auto handle = pool.acquire(first);
    ASSERT_TRUE(handle);
    EXPECT_EQ(pool.active_count(), 1U);
    EXPECT_EQ(pool.available(), 0U);
    const auto exhausted = pool.acquire(Order{});
    ASSERT_FALSE(exhausted);
    EXPECT_EQ(exhausted.error().code, "POOL_EXHAUSTED");
    ASSERT_NE(pool.get(handle.value()), nullptr);
    EXPECT_TRUE(pool.release(handle.value()));
    EXPECT_FALSE(pool.release(handle.value()));
    EXPECT_EQ(pool.get(handle.value()), nullptr);
    Order second{};
    second.id = 12;
    const auto reused = pool.acquire(second);
    ASSERT_TRUE(reused);
    EXPECT_EQ(reused.value().index, handle.value().index);
    EXPECT_NE(reused.value().generation, handle.value().generation);
    ASSERT_NE(pool.get(reused.value()), nullptr);
    EXPECT_EQ(pool.get(reused.value())->id, 12U);
}

TEST(ArenaOrderBookTest, PoolExhaustionAndReclamationPreserveInvariants) {
    ArenaOrderBook book(1);
    Order first{1, Side::Buy, OrderType::Limit, 100, 3, 3, 1, 1};
    ASSERT_TRUE(book.insert_resting_order(first));
    Order overflow{2, Side::Buy, OrderType::Limit, 99, 1, 1, 2, 2};
    EXPECT_FALSE(book.insert_resting_order(overflow));
    EXPECT_EQ(book.last_error(), "POOL_EXHAUSTED");
    EXPECT_EQ(book.active_order_count(), 1U);
    EXPECT_EQ(book.pool_available(), 0U);
    ASSERT_TRUE(book.cancel_order(1).success);
    EXPECT_EQ(book.pool_available(), 1U);
    overflow.sequence = 3;
    ASSERT_TRUE(book.insert_resting_order(overflow));
    EXPECT_TRUE(book.verify_invariants());
    book.clear();
    EXPECT_EQ(book.active_order_count(), 0U);
    EXPECT_EQ(book.pool_available(), 1U);
}

TEST(PipelineTelemetryTest, SamplesBoundedSpansAndReportsStagesQueueQualityAndDrops) {
    PipelineTelemetry telemetry(2, 2, "steady_clock_test", 5);
    TelemetrySpan span{};
    for (std::size_t i = 0; i < span.timestamps_ns.size(); ++i)
        span.timestamps_ns[i] = static_cast<std::int64_t>(100 + i * 10);
    EXPECT_TRUE(telemetry.record(span));
    EXPECT_TRUE(telemetry.record(span)); // sampled out
    EXPECT_TRUE(telemetry.record(span));
    EXPECT_TRUE(telemetry.record(span)); // sampled out
    EXPECT_FALSE(telemetry.record(span)); // fixed storage is full
    auto invalid = span;
    invalid.timestamps_ns[5] = 1;
    EXPECT_FALSE(telemetry.record(invalid));
    telemetry.record_queue_depth(8);
    telemetry.record_queue_depth(3);
    telemetry.record_quality(common::FeedQualityState::Ok);
    telemetry.record_quality(common::FeedQualityState::Stale);
    const auto snapshot = telemetry.snapshot();
    EXPECT_EQ(snapshot.clock_source, "steady_clock_test");
    EXPECT_EQ(snapshot.clock_resolution_ns, 5U);
    EXPECT_EQ(snapshot.recorded_spans, 2U);
    EXPECT_EQ(snapshot.sampled_out_spans, 2U);
    EXPECT_EQ(snapshot.dropped_spans, 2U);
    EXPECT_EQ(snapshot.queue_high_water, 8U);
    EXPECT_EQ(snapshot.quality_counts[0], 1U);
    EXPECT_EQ(snapshot.quality_counts[1], 1U);
    ASSERT_EQ(snapshot.latency.size(), 11U);
    EXPECT_EQ(snapshot.latency[0].stage, "T0-T1");
    EXPECT_EQ(snapshot.latency[0].p50_ns, 10U);
    EXPECT_EQ(snapshot.latency[0].p999_ns, 10U);
    EXPECT_EQ(snapshot.latency[10].stage, "T0-T10");
    EXPECT_EQ(snapshot.latency[10].p50_ns, 100U);
}

TEST(BenchmarkRunnerTest, RunsTwelveProfilesAndChecksThreeBookImplementationsForParity) {
    const BenchmarkRunner runner;
    const auto multi = runner.run(WorkloadProfile::MultiInstrument50, 250);
    ASSERT_EQ(multi.size(), 3U);
    EXPECT_EQ(multi[0].implementation, "StandardOrderBook (map/list)");
    EXPECT_EQ(multi[1].implementation, "BaselineOrderBook (vector)");
    EXPECT_EQ(multi[2].implementation, "ArenaOrderBook (pooled nodes)");
    EXPECT_EQ(multi[2].pool_exhaustions, 0U);
    const auto all = runner.run_all(100, true);
    EXPECT_EQ(all.size(), 32U); // ten book profiles × three implementations, two bus profiles
}

TEST(BenchmarkRunnerTest, ReportsMarketBurstAndSlowConsumerQueueOutcomes) {
    const BenchmarkRunner runner;
    const auto burst = runner.run(WorkloadProfile::MarketDataBurst, 100);
    ASSERT_EQ(burst.size(), 1U);
    EXPECT_EQ(burst[0].events_accepted, 100U);
    EXPECT_EQ(burst[0].events_dropped, 0U);
    EXPECT_EQ(burst[0].queue_high_water, 100U);
    const auto slow = runner.run(WorkloadProfile::SlowConsumerBackpressure, 100);
    ASSERT_EQ(slow.size(), 1U);
    EXPECT_EQ(slow[0].events_accepted, 64U);
    EXPECT_EQ(slow[0].events_dropped, 36U);
    EXPECT_EQ(slow[0].queue_high_water, 64U);
}

TEST(BenchmarkRunnerTest, ArenaPressureReportsCapacityAndDeterministicExhaustion) {
    const BenchmarkRunner runner;
    const auto pressure = runner.run_arena_pressure(100, 10);
    EXPECT_EQ(pressure.orders_processed, 100U);
    EXPECT_EQ(pressure.pool_capacity, 10U);
    EXPECT_EQ(pressure.pool_allocations, 10U);
    EXPECT_EQ(pressure.pool_peak_active, 10U);
    EXPECT_EQ(pressure.pool_exhaustions, 90U);
}

TEST(OrderBookPropertyTest, SeededRandomSequencesPreserveParityPriorityAndConservation) {
    for (const auto profile : {WorkloadProfile::Balanced, WorkloadProfile::AggressiveMatching,
                               WorkloadProfile::CancelHeavy, WorkloadProfile::WideDistribution}) {
        for (const auto seed : {17ULL, 2026ULL}) assert_randomized_book_parity(profile, seed);
    }
}

} // namespace basalt::bench
