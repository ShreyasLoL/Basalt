#include "basalt/backtest/backtest.hpp"
#include "basalt/strategy/builtins.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace basalt;

backtest::BacktestEvent event(const std::uint64_t sequence, const std::int64_t open_time,
                              const std::int64_t close_time, const std::int64_t open,
                              const std::int64_t high, const std::int64_t low,
                              const std::int64_t close) {
    backtest::BacktestEvent record{};
    record.sequence = sequence;
    record.open_time_ns = open_time;
    record.close_time_ns = close_time;
    record.session_key = "SIM-SESSION-1";
    record.event.event_id = "fixture-" + std::to_string(sequence);
    record.event.instrument_id = 7;
    record.event.kind = market_data::MarketEventKind::Bar;
    record.event.event_time_ns = close_time;
    record.event.receive_time_utc_ns = close_time;
    record.event.source = common::DataSourceType::Historical;
    record.event.quality = common::FeedQualityState::Ok;
    record.event.bar = market_data::OhlcvBar{open, high, low, close, 100};
    return record;
}
backtest::BacktestConfig config() {
    backtest::BacktestConfig value{};
    value.dataset_id = "fixture-bars";
    value.dataset_sha256 = std::string(64, 'a');
    value.data_source = "synthetic fixture";
    value.license_tag = "TEST";
    value.corporate_action_policy = "unadjusted fixture";
    value.instrument_master_version = 1;
    value.instrument.id = 7;
    value.instrument.symbol = "SIMTEST";
    value.instrument.display_symbol = "SIMTEST";
    value.instrument.segment = "SIM";
    value.instrument.tick_size_micro = 1;
    value.instrument.lot_size = 1;
    value.account_id = 11;
    value.strategy_id = 22;
    value.initial_cash_micro = 1'000'000;
    value.exchange_calendar = "SIM-24x7";
    value.seed = 1337;
    value.execution.model = trading::PaperExecutionModel::BarApproximation;
    value.execution.tick_size_micro = 1;
    value.execution.slippage_ticks = 1;
    value.execution.participation_bps = 10'000;
    value.half_spread_micro = 1;
    value.splits = {{1, 50}, {100, 200}, {300, 2'000}, 0};
    value.selected_split = backtest::DataSplit::Test;
    value.bar_interval_ns = 100;
    return value;
}

std::vector<backtest::BacktestEvent> bars() {
    return {event(1, 1'000, 1'099, 10, 10, 10, 10),
            event(2, 1'100, 1'199, 10, 10, 10, 10),
            event(3, 1'200, 1'299, 20, 20, 20, 20),
            event(4, 1'300, 1'399, 21, 23, 20, 22)};
}

double metric(const backtest::AnalyticsReport& report, const std::string& name) {
    const auto found = std::find_if(report.metrics.begin(), report.metrics.end(), [&](const auto& value) {
        return value.name == name;
    });
    EXPECT_NE(found, report.metrics.end());
    return found == report.metrics.end() || !found->value ? 0.0 : *found->value;
}

} // namespace

TEST(BacktestSplitTest, RequiresChronologicalDisjointRangesAndEmbargo) {
    backtest::DatasetSplits splits{{1, 100}, {110, 200}, {210, 300}, 10};
    EXPECT_TRUE(backtest::validate_splits(splits));
    splits.test.start_ns = 205;
    EXPECT_EQ(backtest::validate_splits(splits).error().code, "SPLIT_EMBARGO");
    splits.test.start_ns = 190;
    EXPECT_EQ(backtest::validate_splits(splits).error().code, "SPLIT_OVERLAP");
}

TEST(ReplayControllerTest, SupportsPauseStepPlaySpeedAndCheckpointSeek) {
    auto source = bars();
    backtest::ReplayController replay(source);
    ASSERT_TRUE(replay.validate());
    EXPECT_FALSE(replay.next());
    auto first = replay.step_one();
    ASSERT_TRUE(first);
    EXPECT_EQ(replay.current_sequence(), 1U);
    EXPECT_EQ(replay.virtual_time_ns(), 1'099);
    EXPECT_EQ(replay.step_one().value().sequence, 2U);
    std::vector<std::uint64_t> applied;
    const std::vector<backtest::ReplayCheckpoint> checkpoints{{2, 1'199, "verified-hash", "state-at-2"}};
    auto sought = replay.seek(3, checkpoints,
        [](const backtest::ReplayCheckpoint& point) {
            return point.state_hash == "verified-hash" ? common::Result<void>::success()
                : common::Result<void>::failure({"CHECKPOINT_INVALID", "bad hash"});
        },
        [&](const backtest::BacktestEvent& record) {
            applied.push_back(record.sequence);
            return common::Result<void>::success();
        });
    ASSERT_TRUE(sought);
    EXPECT_EQ(applied, (std::vector<std::uint64_t>{3}));
    EXPECT_EQ(replay.current_sequence(), 3U);
    EXPECT_EQ(replay.virtual_time_ns(), 1'299);
    EXPECT_EQ(replay.step_one().value().sequence, 4U);
    replay.set_speed(backtest::ReplaySpeed::TenX);
    EXPECT_EQ(replay.speed(), backtest::ReplaySpeed::TenX);
    EXPECT_EQ(backtest::to_string(replay.speed()), "10x");
    replay.play();
    EXPECT_TRUE(replay.playing());
    EXPECT_FALSE(replay.next()); // End-of-stream remains a clean empty read.
}

TEST(ReplayControllerTest, RejectsDuplicateAndOutOfOrderEvents) {
    auto source = bars();
    source[2].sequence = source[1].sequence;
    backtest::ReplayController duplicate(std::move(source));
    EXPECT_EQ(duplicate.validate().error().code, "REPLAY_DUPLICATE");
    auto unordered = bars();
    std::swap(unordered[1], unordered[2]);
    backtest::ReplayController out_of_order(std::move(unordered));
    EXPECT_EQ(out_of_order.validate().error().code, "REPLAY_SEQUENCE");
}

TEST(AnalyticsTest, ReportsEighteenMetricsAndPreservesUndefinedStatuses) {
    backtest::AnalyticsInput input{};
    input.equity_curve = {{1'000'000'000, 100}, {2'000'000'000, 110},
                          {3'000'000'000, 100}, {4'000'000'000, 120}};
    input.exposure_curve = {{1'000'000'000, 0}, {2'000'000'000, 50},
                            {3'000'000'000, 25}, {4'000'000'000, 75}};
    input.closed_trade_pnl_micro = {100, -50};
    input.execution_count = 4;
    input.turnover_micro = 500;
    input.total_fees_micro = 10;
    input.gross_pre_fee_pnl_micro = 50;
    input.signed_slippage_micro = 4;
    input.absolute_slippage_micro = 8;
    input.peak_gross_exposure_micro = 75;
    input.estimated_capacity_notional_micro = 1'000;
    input.estimated_capacity_quantity = 10;
    input.benchmark_cumulative_return = 0.1;
    const auto report = backtest::calculate_metrics(input);
    ASSERT_EQ(report.metrics.size(), 18U);
    EXPECT_NEAR(metric(report, "Total Return"), 0.2, 1e-12);
    EXPECT_NEAR(metric(report, "Benchmark-Relative Return"), 0.1, 1e-12);
    EXPECT_NEAR(metric(report, "Maximum Drawdown"), 1.0 / 11.0, 1e-12);
    EXPECT_NEAR(metric(report, "Win Rate"), 0.5, 1e-12);
    EXPECT_NEAR(metric(report, "Profit Factor"), 2.0, 1e-12);
    EXPECT_NEAR(metric(report, "Expectancy"), 25.0, 1e-12);
    EXPECT_NEAR(*report.compounded_relative_wealth, 1.2 / 1.1 - 1.0, 1e-12);
    EXPECT_EQ(report.execution_count, 4U);
    EXPECT_EQ(report.fee_to_gross_pnl_ratio, 0.2);

    backtest::AnalyticsInput empty{};
    const auto undefined = backtest::calculate_metrics(empty);
    ASSERT_EQ(undefined.metrics.size(), 18U);
    EXPECT_FALSE(undefined.metrics[0].value);
    EXPECT_NE(undefined.metrics[0].status, "OK");

    auto find_metric = [](const backtest::AnalyticsReport& target, const std::string& name) {
        const auto it = std::find_if(target.metrics.begin(), target.metrics.end(),
                                     [&](const auto& item) { return item.name == name; });
        EXPECT_NE(it, target.metrics.end());
        return *it;
    };

    backtest::AnalyticsInput losses_only = input;
    losses_only.closed_trade_pnl_micro = {-50, -25};
    const auto losses_report = backtest::calculate_metrics(losses_only);
    ASSERT_EQ(losses_report.metrics.size(), 18U);
    const auto no_wins_metric = find_metric(losses_report, "Average Win/Loss");
    EXPECT_FALSE(no_wins_metric.value.has_value());
    EXPECT_EQ(no_wins_metric.status, "NO_WINS");

    backtest::AnalyticsInput wins_only = input;
    wins_only.closed_trade_pnl_micro = {100};
    const auto wins_report = backtest::calculate_metrics(wins_only);
    ASSERT_EQ(wins_report.metrics.size(), 18U);
    const auto no_losses_metric = find_metric(wins_report, "Average Win/Loss");
    EXPECT_FALSE(no_losses_metric.value.has_value());
    EXPECT_EQ(no_losses_metric.status, "NO_AVERAGE_LOSS");
}

TEST(BacktestEngineTest, FillsOnlyAtTheNextBarOpenAndReplaysDeterministically) {
    auto settings = config();
    strategy::StrategyConfig strategy_config{};
    strategy_config.order_quantity = 1;
    strategy_config.integer_parameters = {{"fast_period", 1}, {"slow_period", 2}};
    backtest::BacktestEngine engine;
    auto first = engine.run(strategy::make_builtin(strategy::BuiltinKind::SmaCrossover),
                            strategy_config, bars(), settings);
    ASSERT_TRUE(first) << first.error().code << ": " << first.error().message;
    EXPECT_TRUE(first.value().valid);
    ASSERT_EQ(first.value().execution_count, 1U);
    EXPECT_EQ(first.value().completed_orders, 1U);
    ASSERT_EQ(first.value().signals.size(), 1U);
    EXPECT_TRUE(first.value().signals[0].approved);
    ASSERT_EQ(first.value().orders.size(), 1U);
    EXPECT_EQ(first.value().orders[0].status, "FILLED");
    EXPECT_EQ(first.value().position_journal.size(), 4U);
    EXPECT_EQ(first.value().fills.back().time_ns, 1'300);
    EXPECT_EQ(first.value().fills.back().price_micro, 23);
    EXPECT_GT(first.value().fills.back().time_ns, 1'299);
    EXPECT_EQ(first.value().analytics.metrics.size(), 18U);
    EXPECT_EQ(first.value().fills.back().source, common::DataSourceType::Historical);
    EXPECT_TRUE(first.value().manifest_json.find("SIM-24x7") != std::string::npos);
    EXPECT_FALSE(first.value().warnings.empty());
    ASSERT_FALSE(first.value().manifest_sha256.empty());
    ASSERT_FALSE(first.value().output_sha256.empty());

    auto second = engine.run(strategy::make_builtin(strategy::BuiltinKind::SmaCrossover),
                             strategy_config, bars(), settings);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.value().input_event_sha256, second.value().input_event_sha256);
    EXPECT_EQ(first.value().output_sha256, second.value().output_sha256);
    EXPECT_EQ(first.value().manifest_sha256, second.value().manifest_sha256);
}

TEST(BacktestEngineTest, RejectsInputThatCouldIntroduceLookaheadOrDatasetLeakage) {
    auto settings = config();
    strategy::StrategyConfig strategy_config{};
    strategy_config.integer_parameters = {{"fast_period", 1}, {"slow_period", 2}};
    auto source = bars();
    source[3].open_time_ns = source[2].open_time_ns;
    source[3].close_time_ns = source[2].close_time_ns;
    source[3].event.event_time_ns = source[2].close_time_ns;
    backtest::BacktestEngine engine;
    auto result = engine.run(strategy::make_builtin(strategy::BuiltinKind::SmaCrossover),
                             strategy_config, source, settings);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, "BACKTEST_EVENT_ORDER");
}

TEST(BacktestEngineTest, IncludesSeedPositionsAndKeepsOpenTradesOutOfClosedTradeMetrics) {
    auto settings = config();
    settings.initial_positions = {{7, -1, 10}};
    settings.allow_short = true;
    settings.initial_cash_micro = 1'000'000;
    strategy::StrategyConfig strategy_config{};
    strategy_config.integer_parameters = {{"fast_period", 1}, {"slow_period", 2}};
    backtest::BacktestEngine engine;
    auto result = engine.run(strategy::make_builtin(strategy::BuiltinKind::SmaCrossover),
                             strategy_config, bars(), settings);
    ASSERT_TRUE(result) << result.error().code << ": " << result.error().message;
    EXPECT_EQ(result.value().fills.size(), 2U); // one seed plus one strategy execution
    EXPECT_EQ(result.value().execution_count, 1U);
    EXPECT_TRUE(result.value().closed_trade_pnl_micro.empty());
}

TEST(BacktestEngineTest, JournalsRiskRejectionsWithoutCreatingExecutableOrders) {
    auto settings = config();
    settings.risk_limits.max_order_notional_micro = 10;
    strategy::StrategyConfig strategy_config{};
    strategy_config.integer_parameters = {{"fast_period", 1}, {"slow_period", 2}};
    backtest::BacktestEngine engine;
    auto result = engine.run(strategy::make_builtin(strategy::BuiltinKind::SmaCrossover),
                             strategy_config, bars(), settings);
    ASSERT_TRUE(result) << result.error().code << ": " << result.error().message;
    EXPECT_EQ(result.value().rejected_orders, 1U);
    EXPECT_EQ(result.value().execution_count, 0U);
    ASSERT_EQ(result.value().signals.size(), 1U);
    EXPECT_FALSE(result.value().signals[0].approved);
    ASSERT_EQ(result.value().orders.size(), 1U);
    EXPECT_EQ(result.value().orders[0].status, "REJECTED");
    ASSERT_EQ(result.value().risk_rejections.size(), 1U);
    EXPECT_EQ(result.value().risk_rejections[0].first, "ORDER_NOTIONAL_LIMIT");
}

TEST(BacktestEngineTest, InvalidQualityBarsInvalidateTheRun) {
    auto source = bars();
    source[2].event.quality = common::FeedQualityState::Stale;
    strategy::StrategyConfig strategy_config{};
    strategy_config.integer_parameters = {{"fast_period", 1}, {"slow_period", 2}};
    backtest::BacktestEngine engine;
    auto result = engine.run(strategy::make_builtin(strategy::BuiltinKind::SmaCrossover),
                             strategy_config, source, config());
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, "BACKTEST_DATA_QUALITY");
}

TEST(BacktestEngineTest, FillTimeGapUpCashShortfallRejectsOrderWithoutAbortingRun) {
    auto settings = config();
    // Signal at bar 3 close sees price 20 (approved against 25 micro-units cash),
    // then bar 4 gaps up to open=50 so fill price is 52 > 25.
    settings.initial_cash_micro = 25;
    auto gap_bars = bars();
    gap_bars[3] = event(4, 1'300, 1'399, 50, 55, 48, 52);

    strategy::StrategyConfig strategy_config{};
    strategy_config.order_quantity = 1;
    strategy_config.integer_parameters = {{"fast_period", 1}, {"slow_period", 2}};

    backtest::BacktestEngine engine;
    auto first = engine.run(strategy::make_builtin(strategy::BuiltinKind::SmaCrossover),
                            strategy_config, gap_bars, settings);
    ASSERT_TRUE(first) << first.error().code << ": " << first.error().message;
    EXPECT_TRUE(first.value().valid);
    EXPECT_EQ(first.value().rejected_orders, 1U);
    EXPECT_EQ(first.value().completed_orders, 0U);
    EXPECT_EQ(first.value().execution_count, 0U);
    EXPECT_TRUE(first.value().fills.empty());
    EXPECT_TRUE(first.value().cash_journal.empty());
    ASSERT_EQ(first.value().orders.size(), 1U);
    EXPECT_EQ(first.value().orders[0].status, "REJECTED");
    EXPECT_EQ(first.value().orders[0].reason, "FILL_TIME_INSUFFICIENT_CASH");
    EXPECT_EQ(first.value().orders[0].filled_quantity, 0U);
    EXPECT_EQ(first.value().orders[0].remaining_quantity, 1U);
    ASSERT_EQ(first.value().risk_rejections.size(), 1U);
    EXPECT_EQ(first.value().risk_rejections[0],
              (std::pair<std::string, std::uint64_t>{"FILL_TIME_INSUFFICIENT_CASH", 1U}));

    auto second = engine.run(strategy::make_builtin(strategy::BuiltinKind::SmaCrossover),
                             strategy_config, gap_bars, settings);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.value().input_event_sha256, second.value().input_event_sha256);
    EXPECT_EQ(first.value().output_sha256, second.value().output_sha256);
    EXPECT_EQ(first.value().manifest_sha256, second.value().manifest_sha256);
}
