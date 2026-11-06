#include "basalt/bench/benchmark_runner.hpp"
#include "basalt/bench/telemetry.hpp"
#include "basalt/core/arena_order_book.hpp"
#include "basalt/core/object_pool.hpp"

#include <gtest/gtest.h>

namespace basalt::bench {

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

} // namespace basalt::bench
