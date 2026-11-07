#include "basalt/common/fixed_math.hpp"
#include "basalt/strategy/builtins.hpp"
#include "basalt/strategy/indicators.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using namespace basalt;

std::uint64_t legacy_clamped_ratio_bps(const std::uint64_t numerator,
                                       const std::uint64_t denominator) noexcept {
    if (denominator == 0 || numerator == 0) return 0;
    if (numerator >= denominator) return 10'000;
    std::uint64_t remainder = 0;
    std::uint64_t result = 0;
    const auto complement = denominator - numerator;
    for (std::uint32_t i = 0; i < 10'000; ++i) {
        if (remainder >= complement) {
            remainder -= complement;
            ++result;
        } else {
            remainder += numerator;
        }
    }
    return result;
}

std::optional<std::int64_t> legacy_ratio_bps(const std::uint64_t numerator,
                                             const std::uint64_t denominator) noexcept {
    if (denominator == 0) return std::nullopt;
    const auto whole = numerator / denominator;
    if (whole > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 10'000))
        return std::numeric_limits<std::int64_t>::max();
    const auto remainder = numerator % denominator;
    std::uint64_t carry = 0;
    std::uint64_t fraction = 0;
    const auto complement = denominator - remainder;
    for (std::uint32_t i = 0; i < 10'000; ++i) {
        if (carry >= complement) {
            carry -= complement;
            ++fraction;
        } else {
            carry += remainder;
        }
    }
    return static_cast<std::int64_t>(whole * 10'000 + fraction);
}

strategy::StrategyContext context() {
    strategy::StrategyContext value{};
    value.instrument.id = 7;
    value.instrument.symbol = "SIMTEST";
    value.instrument.display_symbol = "SIMTEST";
    value.instrument.tick_size_micro = 1;
    value.instrument.lot_size = 1;
    value.account_id = 11;
    value.strategy_id = 22;
    value.order_quantity = 2;
    value.now_ns = 1'000'000'000;
    value.risk_snapshot.cash_available_micro = 10'000'000'000LL;
    value.risk_snapshot.equity_micro = 10'000'000'000LL;
    value.risk_snapshot.high_water_mark_micro = 10'000'000'000LL;
    value.risk_snapshot.bid_micro = 99;
    value.risk_snapshot.ask_micro = 100;
    return value;
}

market_data::CanonicalMarketEvent bar(const std::int64_t close, const std::int64_t time,
                                      const std::uint64_t volume = 100) {
    market_data::CanonicalMarketEvent event{};
    event.event_id = "bar-" + std::to_string(time);
    event.instrument_id = 7;
    event.kind = market_data::MarketEventKind::Bar;
    event.event_time_ns = time;
    event.receive_time_utc_ns = time;
    event.quality = common::FeedQualityState::Ok;
    event.source = common::DataSourceType::Synthetic;
    event.bar = market_data::OhlcvBar{close, close, close, close, volume};
    return event;
}

market_data::CanonicalMarketEvent depth(const std::int64_t time) {
    auto event = bar(100, time);
    event.kind = market_data::MarketEventKind::Depth;
    event.bar.reset();
    event.bid_micro = 99;
    event.ask_micro = 100;
    event.bids = {{99, 99, 80}, {98, 98, 20}};
    event.asks = {{100, 100, 10}, {101, 101, 10}};
    return event;
}

std::vector<strategy::StrategySignal> run_bars(strategy::IStrategy& instance,
                                                strategy::StrategyContext ctx,
                                                const std::vector<std::int64_t>& prices,
                                                std::uint64_t volume = 100) {
    std::vector<strategy::StrategySignal> output;
    auto time = ctx.now_ns - static_cast<std::int64_t>(prices.size()) * 1'000;
    for (const auto price : prices) {
        auto event = bar(price, time, volume);
        ctx.now_ns = time;
        auto result = instance.on_event(event, ctx);
        EXPECT_TRUE(result) << (result ? "" : result.error().message);
        if (result) for (auto& signal : result.value()) output.push_back(std::move(signal));
        time += 1'000;
    }
    return output;
}

class ThrowingStrategy final : public strategy::IStrategy {
public:
    strategy::StrategyDescriptor descriptor() const override {
        return {"THROW", "Throw fixture", "1", "fixture-hash", 0,
                {market_data::MarketEventKind::Bar}, {}, {}};
    }
    common::Result<void, strategy::StrategyError> configure(const strategy::StrategyConfig&) override {
        return common::Result<void, strategy::StrategyError>::success();
    }
    common::Result<void, strategy::StrategyError> initialise(const strategy::StrategyContext&) override {
        return common::Result<void, strategy::StrategyError>::success();
    }
    common::Result<std::vector<strategy::StrategySignal>, strategy::StrategyError> on_event(
        const market_data::CanonicalMarketEvent&, const strategy::StrategyContext&) override {
        throw std::runtime_error("fixture failure");
    }
    void on_execution(const trading::ExecutionReport&, const strategy::StrategyContext&) override {}
    void on_stop() noexcept override { stopped = true; }
    bool stopped{};
};

} // namespace

TEST(IndicatorsTest, SimpleMovingAverageUsesFixedCapacityRollingWindow) {
    strategy::SimpleMovingAverage indicator(3);
    ASSERT_TRUE(indicator.update(10));
    ASSERT_TRUE(indicator.update(20));
    EXPECT_FALSE(indicator.value());
    ASSERT_TRUE(indicator.update(30));
    EXPECT_EQ(indicator.value(), 20);
    ASSERT_TRUE(indicator.update(40));
    EXPECT_EQ(indicator.value(), 30);
    EXPECT_EQ(indicator.samples(), 3U);
}

TEST(IndicatorsTest, ExponentialMovingAverageUsesAlphaTwoOverPeriodPlusOne) {
    strategy::ExponentialMovingAverage indicator(3);
    ASSERT_TRUE(indicator.update(10));
    ASSERT_TRUE(indicator.update(20));
    EXPECT_FALSE(indicator.value());
    ASSERT_TRUE(indicator.update(20));
    EXPECT_EQ(indicator.value(), 17);
    ASSERT_TRUE(indicator.update(20));
    EXPECT_EQ(indicator.value(), 18);
}

TEST(IndicatorsTest, WilderRsiDefinesFlatGainAndLossCases) {
    strategy::RelativeStrengthIndex rising(2);
    ASSERT_TRUE(rising.update(10));
    ASSERT_TRUE(rising.update(11));
    ASSERT_TRUE(rising.update(12));
    EXPECT_EQ(rising.value_bps(), 10'000);
    strategy::RelativeStrengthIndex falling(2);
    ASSERT_TRUE(falling.update(12));
    ASSERT_TRUE(falling.update(11));
    ASSERT_TRUE(falling.update(10));
    EXPECT_EQ(falling.value_bps(), 0);
    strategy::RelativeStrengthIndex flat(2);
    ASSERT_TRUE(flat.update(100));
    ASSERT_TRUE(flat.update(100));
    ASSERT_TRUE(flat.update(100));
    EXPECT_EQ(flat.value_bps(), 5'000);
}

TEST(IndicatorsTest, BollingerUsesPopulationVarianceAndIntegerSquareRoot) {
    strategy::BollingerBandsIndicator indicator(3, 20'000);
    ASSERT_TRUE(indicator.update(10));
    ASSERT_TRUE(indicator.update(20));
    ASSERT_TRUE(indicator.update(30));
    ASSERT_TRUE(indicator.value());
    EXPECT_EQ(indicator.value()->middle, 20);
    EXPECT_EQ(indicator.value()->upper, 36);
    EXPECT_EQ(indicator.value()->lower, 4);
}

TEST(IndicatorsTest, SessionVwapResetsOnlyWhenCallerChangesSessionKey) {
    strategy::SessionVwap indicator;
    ASSERT_TRUE(indicator.update(100, 1, "2026-11-07"));
    ASSERT_TRUE(indicator.update(200, 3, "2026-11-07"));
    EXPECT_EQ(indicator.value(), 175);
    ASSERT_TRUE(indicator.update(50, 2, "next-session"));
    EXPECT_EQ(indicator.value(), 50);
}

TEST(IndicatorsTest, BookImbalanceIsSignedAndUnavailableForEmptyBook) {
    strategy::BookImbalance indicator(5);
    EXPECT_EQ(indicator.value_bps(80, 20), 6'000);
    EXPECT_EQ(indicator.value_bps(20, 80), -6'000);
    EXPECT_FALSE(indicator.value_bps(0, 0));
}

TEST(IndicatorsTest, ArithmeticOverflowAndInvalidPeriodsAreReported) {
    strategy::SimpleMovingAverage bad_period(0);
    EXPECT_EQ(bad_period.update(1).error().code, "INVALID_PERIOD");
    strategy::SimpleMovingAverage overflow(2);
    ASSERT_TRUE(overflow.update(std::numeric_limits<std::int64_t>::max()));
    EXPECT_EQ(overflow.update(1).error().code, "ARITHMETIC_OVERFLOW");
}

TEST(FixedMathTest, RatioBpsMatchesLegacyLoopAcrossEdgeCasesAndSeededRandomRange) {
    constexpr auto u64_max = std::numeric_limits<std::uint64_t>::max();
    constexpr auto whole_cap =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 10'000);

    const std::vector<std::pair<std::uint64_t, std::uint64_t>> edge_cases = {
        {0, 0},
        {1, 0},
        {u64_max, 0},
        {0, 1},
        {0, 10'000},
        {0, u64_max},
        {1, 1},
        {1, 2},
        {1, 3},
        {2, 3},
        {9'999, 10'000},
        {10'000, 10'000},
        {10'001, 10'000},
        {1, u64_max},
        {u64_max / 2, u64_max},
        {u64_max - 1, u64_max},
        {u64_max, u64_max},
        {u64_max, u64_max - 1},
        {whole_cap, 1},
        {whole_cap + 1, 1},
        {u64_max, 1},
        {whole_cap * 3 + 2, 3},
        {(whole_cap + 1) * 3, 3},
    };

    for (const auto& [numerator, denominator] : edge_cases) {
        EXPECT_EQ(common::clamped_ratio_bps(numerator, denominator),
                  legacy_clamped_ratio_bps(numerator, denominator))
            << "numerator=" << numerator << " denominator=" << denominator;
        EXPECT_EQ(common::ratio_bps(numerator, denominator),
                  legacy_ratio_bps(numerator, denominator))
            << "numerator=" << numerator << " denominator=" << denominator;
        if (denominator != 0) {
            const auto rem = numerator % denominator;
            EXPECT_EQ(common::detail::scale_remainder_bps_portable(rem, denominator),
                      common::detail::scale_remainder_bps(rem, denominator))
                << "rem=" << rem << " denominator=" << denominator;
        }
    }

    std::mt19937_64 rng(0xBA5A1706ULL);
    for (int i = 0; i < 2'500; ++i) {
        const auto a = rng();
        const auto b = rng();
        const auto denominator = b == 0 ? 1ULL : b;
        const auto fractional_num = a % denominator;

        EXPECT_EQ(common::clamped_ratio_bps(fractional_num, denominator),
                  legacy_clamped_ratio_bps(fractional_num, denominator));
        EXPECT_EQ(common::ratio_bps(fractional_num, denominator),
                  legacy_ratio_bps(fractional_num, denominator));
        EXPECT_EQ(common::detail::scale_remainder_bps_portable(fractional_num, denominator),
                  common::detail::scale_remainder_bps(fractional_num, denominator));

        EXPECT_EQ(common::clamped_ratio_bps(a, denominator),
                  legacy_clamped_ratio_bps(a, denominator));
        EXPECT_EQ(common::ratio_bps(a, denominator),
                  legacy_ratio_bps(a, denominator));
        EXPECT_EQ(common::detail::scale_remainder_bps_portable(a % denominator, denominator),
                  common::detail::scale_remainder_bps(a % denominator, denominator));
    }
}

TEST(BuiltinStrategyTest, FactoryExposesSixStableDescriptorsAndRejectsUnknownParameters) {
    for (std::uint8_t i = 0; i <= static_cast<std::uint8_t>(strategy::BuiltinKind::OrderBookImbalance); ++i) {
        auto instance = strategy::make_builtin(static_cast<strategy::BuiltinKind>(i));
        const auto descriptor = instance->descriptor();
        EXPECT_FALSE(descriptor.strategy_id.empty());
        EXPECT_FALSE(descriptor.stable_hash.empty());
        EXPECT_FALSE(descriptor.required_channels.empty());
        EXPECT_FALSE(descriptor.parameter_schema.empty());
    }
    auto instance = strategy::make_builtin(strategy::BuiltinKind::SmaCrossover);
    strategy::StrategyConfig invalid{};
    invalid.integer_parameters["not_a_parameter"] = 1;
    EXPECT_EQ(instance->configure(invalid).error().code, "UNKNOWN_STRATEGY_PARAMETER");
}

TEST(BuiltinStrategyTest, SmaCrossoverEmitsDeterministicRiskOnlyOrderIntent) {
    auto first = strategy::make_builtin(strategy::BuiltinKind::SmaCrossover);
    strategy::StrategyConfig config{};
    config.order_quantity = 2;
    config.integer_parameters = {{"fast_period", 2}, {"slow_period", 3}};
    ASSERT_TRUE(first->configure(config));
    auto ctx = context();
    ASSERT_TRUE(first->initialise(ctx));
    const auto signals = run_bars(*first, ctx, {10, 10, 10, 20});
    ASSERT_EQ(signals.size(), 1U);
    EXPECT_EQ(signals[0].intent.side, trading::Side::Buy);
    EXPECT_EQ(signals[0].intent.quantity, 2U);
    EXPECT_EQ(signals[0].intent.instrument_id, 7U);
    EXPECT_NE(signals[0].signal_id, "");

    auto second = strategy::make_builtin(strategy::BuiltinKind::SmaCrossover);
    ASSERT_TRUE(second->configure(config));
    ASSERT_TRUE(second->initialise(ctx));
    const auto repeated = run_bars(*second, ctx, {10, 10, 10, 20});
    ASSERT_EQ(repeated.size(), 1U);
    EXPECT_EQ(repeated[0].signal_id, signals[0].signal_id);
}

TEST(BuiltinStrategyTest, RsiMeanReversionUsesCrossingThresholds) {
    auto instance = strategy::make_builtin(strategy::BuiltinKind::RsiMeanReversion);
    strategy::StrategyConfig config{};
    config.integer_parameters["period"] = 2;
    ASSERT_TRUE(instance->configure(config));
    auto ctx = context();
    ASSERT_TRUE(instance->initialise(ctx));
    const auto signals = run_bars(*instance, ctx, {10, 9, 8, 9});
    ASSERT_EQ(signals.size(), 1U);
    EXPECT_EQ(signals[0].intent.side, trading::Side::Buy);
    EXPECT_EQ(signals[0].values.at("rsi_bps"), 5'000);
}

TEST(BuiltinStrategyTest, BollingerReversionRequiresReentryFromAnExtreme) {
    auto instance = strategy::make_builtin(strategy::BuiltinKind::Bollinger);
    strategy::StrategyConfig config{};
    config.integer_parameters = {{"period", 3}, {"deviation_bps", 5'000}};
    config.text_parameters["mode"] = "reversion";
    ASSERT_TRUE(instance->configure(config));
    auto ctx = context();
    ASSERT_TRUE(instance->initialise(ctx));
    const auto signals = run_bars(*instance, ctx, {100, 100, 1, 50});
    ASSERT_EQ(signals.size(), 1U);
    EXPECT_EQ(signals[0].intent.side, trading::Side::Buy);
}

TEST(BuiltinStrategyTest, VwapReversionRequiresAStableSessionAndMoveTowardVwap) {
    auto instance = strategy::make_builtin(strategy::BuiltinKind::VwapReversion);
    strategy::StrategyConfig config{};
    config.integer_parameters["deviation_threshold_bps"] = 100;
    ASSERT_TRUE(instance->configure(config));
    auto ctx = context();
    ctx.session_key = "session-A";
    ASSERT_TRUE(instance->initialise(ctx));
    const auto signals = run_bars(*instance, ctx, {100, 100, 80, 85}, 10);
    ASSERT_EQ(signals.size(), 1U);
    EXPECT_EQ(signals[0].intent.side, trading::Side::Buy);
    EXPECT_EQ(signals[0].values.at("session_vwap_micro"), 91);
}

TEST(BuiltinStrategyTest, MomentumUsesOnlyPriorCompletedBarsAndVolumeConfirmation) {
    auto instance = strategy::make_builtin(strategy::BuiltinKind::MomentumBreakout);
    strategy::StrategyConfig config{};
    config.integer_parameters = {{"period", 2}, {"volume_period", 2}, {"volume_factor_bps", 10'000}};
    ASSERT_TRUE(instance->configure(config));
    auto ctx = context();
    ASSERT_TRUE(instance->initialise(ctx));
    const auto signals = run_bars(*instance, ctx, {10, 11, 12}, 100);
    ASSERT_EQ(signals.size(), 1U);
    EXPECT_EQ(signals[0].intent.side, trading::Side::Buy);
    EXPECT_EQ(signals[0].values.at("channel_high_micro"), 11);
}

TEST(BuiltinStrategyTest, ImbalanceRequiresFreshQualityDepthAndUsesTopFive) {
    auto instance = strategy::make_builtin(strategy::BuiltinKind::OrderBookImbalance);
    strategy::StrategyConfig config{};
    config.integer_parameters["threshold_bps"] = 1'000;
    ASSERT_TRUE(instance->configure(config));
    auto ctx = context();
    ASSERT_TRUE(instance->initialise(ctx));
    auto event = depth(ctx.now_ns);
    const auto result = instance->on_event(event, ctx);
    ASSERT_TRUE(result);
    ASSERT_EQ(result.value().size(), 1U);
    EXPECT_EQ(result.value()[0].intent.side, trading::Side::Buy);
    EXPECT_EQ(result.value()[0].values.at("imbalance_bps"), 6'666);
}

TEST(BuiltinStrategyTest, FeeAndSlippageAwareBudgetRoundsQuantityDownToLot) {
    auto instance = strategy::make_builtin(strategy::BuiltinKind::SmaCrossover);
    strategy::StrategyConfig config{};
    config.order_quantity = 100;
    config.sizing_budget_micro = 2'000;
    config.fee_bps = 100;
    config.slippage_bps = 100;
    config.integer_parameters = {{"fast_period", 2}, {"slow_period", 3}};
    ASSERT_TRUE(instance->configure(config));
    auto ctx = context();
    ctx.instrument.lot_size = 2;
    ASSERT_TRUE(instance->initialise(ctx));
    const auto signals = run_bars(*instance, ctx, {100, 100, 100, 200});
    ASSERT_EQ(signals.size(), 1U);
    EXPECT_EQ(signals[0].intent.quantity, 8U);
    EXPECT_EQ(signals[0].intent.estimated_fee_micro, 16);
}

TEST(StrategyRuntimeTest, EnforcesTenStateWarmupPauseResumeStopAndRiskBoundary) {
    trading::KillSwitch kill;
    trading::RiskEngine risk(kill);
    auto instance = strategy::make_builtin(strategy::BuiltinKind::SmaCrossover);
    strategy::StrategyRuntime runtime(std::move(instance), risk);
    strategy::StrategyConfig config{};
    config.order_quantity = 2;
    config.integer_parameters = {{"fast_period", 2}, {"slow_period", 3}};
    ASSERT_TRUE(runtime.configure(config));
    auto ctx = context();
    ASSERT_TRUE(runtime.start(ctx));
    EXPECT_EQ(runtime.state(), strategy::LifecycleState::WarmingUp);
    for (const auto [price, time] : {std::pair{10, 999'997'000LL}, std::pair{10, 999'998'000LL},
                                     std::pair{10, 999'999'000LL}}) {
        ctx.now_ns = time;
        auto result = runtime.on_market_event(bar(price, time), ctx);
        ASSERT_TRUE(result);
        EXPECT_TRUE(result.value().approved_signals.empty());
    }
    EXPECT_EQ(runtime.state(), strategy::LifecycleState::Running);
    ctx.now_ns = 1'000'000'000;
    ctx.risk_snapshot.bid_micro = 19;
    ctx.risk_snapshot.ask_micro = 20;
    auto dispatched = runtime.on_market_event(bar(20, ctx.now_ns), ctx);
    ASSERT_TRUE(dispatched);
    ASSERT_EQ(dispatched.value().approved_signals.size(), 1U);
    ASSERT_TRUE(runtime.pause());
    EXPECT_EQ(runtime.state(), strategy::LifecycleState::Paused);
    ASSERT_TRUE(runtime.resume());
    ASSERT_TRUE(runtime.stop());
    EXPECT_EQ(runtime.state(), strategy::LifecycleState::Stopped);
    EXPECT_TRUE(runtime.stop());
    EXPECT_EQ(runtime.transitions().size(), 10U);
}

TEST(StrategyRuntimeTest, RiskRejectionDoesNotLeakAnUnapprovedIntent) {
    trading::KillSwitch kill;
    trading::RiskEngine risk(kill);
    strategy::StrategyRuntime runtime(strategy::make_builtin(strategy::BuiltinKind::SmaCrossover), risk);
    strategy::StrategyConfig config{};
    config.order_quantity = 100;
    config.integer_parameters = {{"fast_period", 2}, {"slow_period", 3}};
    ASSERT_TRUE(runtime.configure(config));
    auto ctx = context();
    ASSERT_TRUE(runtime.start(ctx));
    for (const auto [price, time] : {std::pair{10, 999'997'000LL}, std::pair{10, 999'998'000LL},
                                     std::pair{10, 999'999'000LL}}) {
        ctx.now_ns = time;
        ASSERT_TRUE(runtime.on_market_event(bar(price, time), ctx));
    }
    ctx.now_ns = 1'000'000'000;
    ctx.risk_snapshot.cash_available_micro = 0;
    ctx.risk_snapshot.ask_micro = 20;
    ctx.risk_snapshot.bid_micro = 19;
    auto result = runtime.on_market_event(bar(20, ctx.now_ns), ctx);
    ASSERT_TRUE(result);
    EXPECT_TRUE(result.value().approved_signals.empty());
    ASSERT_EQ(result.value().rejected_signals.size(), 1U);
    EXPECT_EQ(result.value().rejected_signals[0].decision.code, "INSUFFICIENT_CASH");
}

TEST(StrategyRuntimeTest, StaleGappedAndWrongInstrumentEventsDoNotGenerateSignals) {
    trading::KillSwitch kill;
    trading::RiskEngine risk(kill);
    strategy::StrategyRuntime runtime(strategy::make_builtin(strategy::BuiltinKind::SmaCrossover), risk);
    strategy::StrategyConfig config{};
    config.integer_parameters = {{"fast_period", 1}, {"slow_period", 2}};
    ASSERT_TRUE(runtime.configure(config));
    auto ctx = context();
    ASSERT_TRUE(runtime.start(ctx));
    for (const auto time : {999'998'000LL, 999'999'000LL}) {
        ctx.now_ns = time;
        ASSERT_TRUE(runtime.on_market_event(bar(10, time), ctx));
    }
    ctx.now_ns = 1'000'000'000;
    auto gapped = bar(20, ctx.now_ns);
    gapped.sequence_gap = true;
    auto ignored_gap = runtime.on_market_event(gapped, ctx);
    ASSERT_TRUE(ignored_gap);
    EXPECT_TRUE(ignored_gap.value().approved_signals.empty());
    auto stale = bar(20, ctx.now_ns - 10'000'000'000LL);
    auto ignored_stale = runtime.on_market_event(stale, ctx);
    ASSERT_TRUE(ignored_stale);
    EXPECT_TRUE(ignored_stale.value().approved_signals.empty());
    auto wrong = bar(20, ctx.now_ns);
    wrong.instrument_id = 99;
    auto ignored_wrong = runtime.on_market_event(wrong, ctx);
    ASSERT_TRUE(ignored_wrong);
    EXPECT_TRUE(ignored_wrong.value().approved_signals.empty());
}

TEST(StrategyRuntimeTest, ExceptionsAreIsolatedAndTransitionToFailed) {
    trading::KillSwitch kill;
    trading::RiskEngine risk(kill);
    common::StrategyId failed_id = 0;
    std::string failure_code;
    auto fixture = std::make_unique<ThrowingStrategy>();
    strategy::StrategyRuntime runtime(std::move(fixture), risk,
        [&](const common::StrategyId id, const strategy::StrategyError& error) {
            failed_id = id;
            failure_code = error.code;
        });
    ASSERT_TRUE(runtime.configure({}));
    auto ctx = context();
    ASSERT_TRUE(runtime.start(ctx));
    auto event = bar(100, ctx.now_ns);
    auto result = runtime.on_market_event(event, ctx);
    EXPECT_FALSE(result);
    EXPECT_EQ(runtime.state(), strategy::LifecycleState::Failed);
    EXPECT_EQ(runtime.failure()->code, "STRATEGY_EXCEPTION");
    EXPECT_EQ(failed_id, ctx.strategy_id);
    EXPECT_EQ(failure_code, "STRATEGY_EXCEPTION");
    EXPECT_TRUE(runtime.stop());
    EXPECT_EQ(runtime.state(), strategy::LifecycleState::Failed);
}

TEST(StrategyRuntimeTest, IllegalTransitionsAreTypedAndDoNotChangeState) {
    trading::KillSwitch kill;
    trading::RiskEngine risk(kill);
    strategy::StrategyRuntime runtime(strategy::make_builtin(strategy::BuiltinKind::SmaCrossover), risk);
    const auto result = runtime.pause();
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, "ILLEGAL_LIFECYCLE_TRANSITION");
    EXPECT_EQ(runtime.state(), strategy::LifecycleState::Created);
}

