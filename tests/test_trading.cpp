#include "basalt/core/matching_engine.hpp"
#include "basalt/persistence/event_log.hpp"
#include "basalt/trading/execution.hpp"
#include "basalt/trading/ledger.hpp"
#include "basalt/trading/risk.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace basalt::trading {
namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path_ = std::filesystem::current_path() / "phase5-test-temp" /
                ("basalt-phase5-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

OrderIntent intent(std::string id = "order-1") {
    OrderIntent value;
    value.client_order_id = std::move(id);
    value.order_id = 1;
    value.account_id = 1;
    value.instrument_id = 7;
    value.strategy_id = 5;
    value.side = Side::Buy;
    value.type = OrderType::Market;
    value.quantity = 1;
    value.tick_size_micro = 100;
    value.lot_size = 1;
    value.created_at_ns = 1'000'000'000;
    return value;
}

RiskSnapshot risk_snapshot() {
    RiskSnapshot snapshot;
    snapshot.now_ns = 2'000'000'000;
    snapshot.data_age_ns = 1;
    snapshot.cash_available_micro = 100'000'000;
    snapshot.bid_micro = 9'900;
    snapshot.ask_micro = 10'000;
    return snapshot;
}

ExecutionConfig execution_config(const PaperExecutionModel model) {
    ExecutionConfig config;
    config.model = model;
    config.tick_size_micro = 100;
    config.slippage_ticks = 1;
    config.participation_bps = 10'000;
    config.stale_after_ns = 1'000;
    return config;
}

ExecutionMarket quote_market() {
    ExecutionMarket market;
    market.event_sequence = 23;
    market.source = common::DataSourceType::Synthetic;
    market.quality = common::FeedQualityState::Ok;
    market.event_time_ns = 100;
    market.receive_time_ns = 105;
    market.now_ns = 110;
    market.bid_micro = 10'000;
    market.bid_quantity = 20;
    market.ask_micro = 10'100;
    market.ask_quantity = 20;
    market.bids = {{10'000, 20}, {9'900, 30}};
    market.asks = {{10'100, 20}, {10'200, 30}};
    return market;
}

ExecutionFill fill(std::uint64_t execution_id, std::uint64_t sequence, Side side,
                   common::Quantity quantity, common::MicroUnits price,
                   std::int64_t fee = 0, common::AccountId account_id = 1,
                   common::InstrumentId instrument_id = 7) {
    ExecutionFill result;
    result.execution_id = execution_id;
    result.event_sequence = sequence;
    result.order_id = execution_id + 100;
    result.account_id = account_id;
    result.instrument_id = instrument_id;
    result.strategy_id = 5;
    result.side = side;
    result.price_micro = price;
    result.quantity = quantity;
    result.fee_micro = fee;
    result.model = PaperExecutionModel::ConservativeQuote;
    result.source = common::DataSourceType::Synthetic;
    result.quality = common::FeedQualityState::Ok;
    result.fee_schedule_id = fee == 0 ? "NO_FEES_CONFIGURED" : "local-test-v1";
    result.time_ns = static_cast<std::int64_t>(sequence) * 1'000;
    return result;
}

TEST(RiskEngineTest, ExercisesAllSixteenStableRejectionCodes) {
    struct Scenario {
        std::string name;
        std::string expected_code;
        std::function<void(OrderIntent&, RiskSnapshot&, RiskLimits&, KillSwitch&)> change;
    };
    const std::vector<Scenario> scenarios{
        {"order notional", "ORDER_NOTIONAL_LIMIT", [](auto&, auto&, auto& limits, auto&) { limits.max_order_notional_micro = 9'999; }},
        {"order quantity", "ORDER_QUANTITY_LIMIT", [](auto& order, auto&, auto& limits, auto&) { order.quantity = 2; limits.max_order_quantity = 1; }},
        {"position", "POSITION_LIMIT", [](auto&, auto&, auto& limits, auto&) { limits.max_position_quantity = 0; }},
        {"gross exposure", "GROSS_EXPOSURE_LIMIT", [](auto&, auto&, auto& limits, auto&) { limits.max_gross_exposure_micro = 9'999; }},
        {"net exposure", "NET_EXPOSURE_LIMIT", [](auto&, auto&, auto& limits, auto&) { limits.max_net_exposure_micro = 9'999; }},
        {"concentration", "CONCENTRATION_LIMIT", [](auto&, auto&, auto& limits, auto&) { limits.max_concentration_bps = 5'000; }},
        {"daily loss", "DAILY_LOSS_LIMIT", [](auto&, auto& snapshot, auto&, auto&) { snapshot.daily_pnl_micro = -1'000'000'001; }},
        {"drawdown", "DRAWDOWN_LIMIT", [](auto&, auto& snapshot, auto&, auto&) { snapshot.high_water_mark_micro = 2'000'000'000; snapshot.equity_micro = 0; }},
        {"open orders", "OPEN_ORDER_LIMIT", [](auto&, auto& snapshot, auto& limits, auto&) { snapshot.open_orders = limits.max_open_orders; }},
        {"order rate", "ORDER_RATE_LIMIT", [](auto&, auto& snapshot, auto& limits, auto&) { limits.max_orders_per_window = 1; snapshot.recent_order_times_ns.push_back(snapshot.now_ns - 1); }},
        {"instrument", "INSTRUMENT_LIMIT", [](auto&, auto& snapshot, auto&, auto&) { snapshot.instrument_enabled = false; }},
        {"strategy", "STRATEGY_LIMIT", [](auto&, auto& snapshot, auto&, auto&) { snapshot.strategy_enabled = false; }},
        {"stale data", "STALE_DATA", [](auto&, auto& snapshot, auto&, auto&) { snapshot.data_quality_ok = false; }},
        {"session", "MARKET_CLOSED", [](auto&, auto& snapshot, auto&, auto&) { snapshot.market_open = false; }},
        {"duplicate ID", "DUPLICATE_ORDER", [](auto&, auto&, auto&, auto&) {}},
        {"kill switch", "KILL_SWITCH_ACTIVE", [](auto&, auto&, auto&, auto& kill) { (void)kill.activate(KillScope::Global, 0, "test", "risk boundary", 2'000'000'000); }}
    };
    for (const auto& scenario : scenarios) {
        KillSwitch kill;
        RiskEngine engine(kill);
        auto order = intent();
        auto snapshot = risk_snapshot();
        RiskLimits limits;
        scenario.change(order, snapshot, limits, kill);
        if (scenario.expected_code == "DUPLICATE_ORDER") {
            const auto first = engine.evaluate(order, snapshot, limits);
            ASSERT_TRUE(first.accepted) << first.code;
            order.quantity = 2;
        }
        const auto decision = engine.evaluate(order, snapshot, limits);
        EXPECT_FALSE(decision.accepted) << scenario.name;
        EXPECT_EQ(decision.code, scenario.expected_code) << scenario.name;
    }
}

TEST(RiskEngineTest, AcceptedChecksReserveCapacityAndClientIdsAreIdempotent) {
    KillSwitch kill;
    RiskEngine engine(kill);
    auto order = intent();
    auto snapshot = risk_snapshot();
    RiskLimits limits;
    limits.max_position_quantity = 1;
    const auto accepted = engine.evaluate(order, snapshot, limits);
    ASSERT_TRUE(accepted.accepted) << accepted.code;
    EXPECT_EQ(accepted.checks.size(), 16U);
    EXPECT_EQ(engine.reservation_count(), 1U);
    const auto duplicate = engine.evaluate(order, snapshot, limits);
    EXPECT_TRUE(duplicate.accepted);
    EXPECT_TRUE(duplicate.duplicate_retry);
    auto second = intent("order-2");
    second.order_id = 2;
    EXPECT_EQ(engine.evaluate(second, snapshot, limits).code, "POSITION_LIMIT");
    engine.complete(order.client_order_id);
    EXPECT_EQ(engine.reservation_count(), 0U);
    EXPECT_TRUE(engine.evaluate(second, snapshot, limits).accepted);
}

TEST(RiskEngineTest, IdempotentReplayStillFailsOnActiveKillSwitchStaleDataOrClosedMarket) {
    KillSwitch kill;
    RiskEngine engine(kill);
    const auto order = intent("order-replay-gate");
    const auto snapshot = risk_snapshot();
    RiskLimits limits;
    ASSERT_TRUE(engine.evaluate(order, snapshot, limits).accepted);

    // Stale data and closed session must reject a retry before reporting replay.
    auto stale_snapshot = snapshot;
    stale_snapshot.data_quality_ok = false;
    const auto stale_retry = engine.evaluate(order, stale_snapshot, limits);
    EXPECT_FALSE(stale_retry.accepted);
    EXPECT_FALSE(stale_retry.duplicate_retry);
    EXPECT_EQ(stale_retry.code, "STALE_DATA");

    auto aged_snapshot = snapshot;
    aged_snapshot.data_age_ns = limits.max_stale_age_ns + 1U;
    EXPECT_EQ(engine.evaluate(order, aged_snapshot, limits).code, "STALE_DATA");

    auto closed_snapshot = snapshot;
    closed_snapshot.market_open = false;
    const auto closed_retry = engine.evaluate(order, closed_snapshot, limits);
    EXPECT_FALSE(closed_retry.accepted);
    EXPECT_EQ(closed_retry.code, "MARKET_CLOSED");

    // Once conditions are healthy again, the idempotent replay succeeds...
    const auto healthy_retry = engine.evaluate(order, snapshot, limits);
    EXPECT_TRUE(healthy_retry.accepted);
    EXPECT_TRUE(healthy_retry.duplicate_retry);

    // ...until the kill switch latches (strategy, account or global).
    (void)kill.activate(KillScope::Global, 0, "operator", "halt", 3'000'000'000);
    const auto killed_retry = engine.evaluate(order, snapshot, limits);
    EXPECT_FALSE(killed_retry.accepted);
    EXPECT_FALSE(killed_retry.duplicate_retry);
    EXPECT_EQ(killed_retry.code, "KILL_SWITCH_ACTIVE");
}

TEST(RiskEngineTest, InvalidTickMissingQuoteAndOverflowFailClosed) {
    KillSwitch kill;
    RiskEngine engine(kill);
    auto order = intent();
    auto snapshot = risk_snapshot();
    order.type = OrderType::Limit;
    order.limit_price_micro = 10'050;
    EXPECT_EQ(engine.evaluate(order, snapshot).code, "INVALID_TICK");
    order = intent();
    snapshot.ask_micro.reset();
    EXPECT_EQ(engine.evaluate(order, snapshot).code, "DATA_UNAVAILABLE");
    snapshot = risk_snapshot();
    order.quantity = static_cast<common::Quantity>(std::numeric_limits<std::int64_t>::max());
    order.tick_size_micro = std::numeric_limits<std::int64_t>::max();
    snapshot.ask_micro = order.tick_size_micro;
    EXPECT_EQ(engine.evaluate(order, snapshot).code, "RISK_CALCULATION_ERROR");
    order = intent();
    snapshot = risk_snapshot();
    snapshot.daily_pnl_micro = std::numeric_limits<std::int64_t>::min();
    EXPECT_EQ(engine.evaluate(order, snapshot).code, "RISK_CALCULATION_ERROR");
}

TEST(RiskEngineTest, EnforcesAvailableCashAndShortPositionEligibility) {
    KillSwitch kill;
    RiskEngine engine(kill);
    auto order = intent();
    auto snapshot = risk_snapshot();
    snapshot.cash_available_micro = 9'999;
    EXPECT_EQ(engine.evaluate(order, snapshot).code, "INSUFFICIENT_CASH");
    snapshot = risk_snapshot();
    order.side = Side::Sell;
    EXPECT_EQ(engine.evaluate(order, snapshot).code, "INSUFFICIENT_POSITION");
    snapshot.position_quantity = 2;
    const auto allowed_exit = engine.evaluate(order, snapshot);
    EXPECT_TRUE(allowed_exit.accepted) << allowed_exit.code;
}

TEST(RiskEngineTest, PersistsDecisionsAndRestoresClientOrderIdempotency) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "risk.blog";
    persistence::EventLog log(path);
    ASSERT_TRUE(log.recover());
    auto order = intent("durable-order");
    const auto snapshot = risk_snapshot();
    {
        KillSwitch kill;
        RiskEngine engine(kill, RiskEngine::event_log_sink(log));
        EXPECT_TRUE(engine.evaluate(order, snapshot).accepted);
        EXPECT_TRUE(engine.evaluate(order, snapshot).duplicate_retry);
        auto rejected_order = intent("rejected-order");
        auto rejected_snapshot = snapshot;
        rejected_snapshot.market_open = false;
        EXPECT_EQ(engine.evaluate(rejected_order, rejected_snapshot).code, "MARKET_CLOSED");
    }
    persistence::EventLog reopened_log(path);
    ASSERT_TRUE(reopened_log.recover());
    const auto frames = reopened_log.read_all();
    ASSERT_TRUE(frames);
    ASSERT_EQ(frames.value().size(), 3U);
    KillSwitch restarted_kill;
    RiskEngine restarted(restarted_kill);
    ASSERT_TRUE(restarted.restore(frames.value()));
    const auto retry = restarted.evaluate(order, snapshot);
    EXPECT_TRUE(retry.accepted);
    EXPECT_TRUE(retry.duplicate_retry);
    RiskLimits one_per_window;
    one_per_window.max_orders_per_window = 1;
    auto another = intent("new-after-restart");
    another.order_id = 2;
    EXPECT_EQ(restarted.evaluate(another, snapshot, one_per_window).code, "ORDER_RATE_LIMIT");
}

TEST(RiskEngineTest, AuditFailureFailsClosedAndReleasesTheNewReservation) {
    KillSwitch kill;
    RiskEngine engine(kill, [](const std::string&) {
        return common::Result<void>::failure({"DISK_FULL", "simulated full disk"});
    });
    const auto decision = engine.evaluate(intent(), risk_snapshot());
    EXPECT_FALSE(decision.accepted);
    EXPECT_EQ(decision.code, "AUDIT_UNAVAILABLE");
    EXPECT_EQ(engine.reservation_count(), 0U);
}

TEST(KillSwitchTest, PersistsLatchAndRequiresHealthyAuthorizedDurableReset) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "kill.blog";
    persistence::EventLog log(path);
    ASSERT_TRUE(log.recover());
    std::size_t cancelled = 0;
    KillSwitch kill(KillSwitch::event_log_sink(log), [&](const KillScope, const std::uint64_t) {
        ++cancelled;
        return common::Result<void>::success();
    });
    const auto activated = kill.activate(KillScope::Account, 1, "operator", "manual stop", 1'000);
    EXPECT_TRUE(activated.persisted);
    EXPECT_TRUE(activated.working_orders_cancelled);
    EXPECT_EQ(cancelled, 1U);
    EXPECT_TRUE(kill.active(1));
    EXPECT_FALSE(kill.active(2));
    EXPECT_EQ(kill.reset(KillScope::Account, 1, "operator", "unsafe", 2'000, false, true).error().code,
              "KILL_SWITCH_RESET_DENIED");
    EXPECT_EQ(kill.reset(KillScope::Account, 1, "operator", "health not clear", 2'000, true, false).error().code,
              "KILL_SWITCH_RESET_DENIED");
    const auto reset = kill.reset(KillScope::Account, 1, "operator", "health verified", 3'000, true, true);
    ASSERT_TRUE(reset);
    EXPECT_TRUE(reset.value().persisted);
    EXPECT_FALSE(kill.active(1));

    persistence::EventLog reopened_log(path);
    ASSERT_TRUE(reopened_log.recover());
    KillSwitch restarted(KillSwitch::event_log_sink(reopened_log));
    ASSERT_TRUE(restarted.restore(reopened_log.read_all().value()));
    EXPECT_FALSE(restarted.active(1));
    EXPECT_EQ(restarted.events().size(), 2U);
    EXPECT_TRUE(restarted.events()[0].working_orders_cancelled);
}

TEST(KillSwitchTest, LatchesEvenWhenJournalIsUnavailableAndCannotBeReset) {
    persistence::EventLog unrecovered("unrecovered-kill.blog");
    KillSwitch kill(KillSwitch::event_log_sink(unrecovered));
    const auto activated = kill.activate(KillScope::Global, 0, "operator", "fail closed", 1'000);
    EXPECT_FALSE(activated.persisted);
    EXPECT_TRUE(kill.active(99, 88));
    EXPECT_EQ(kill.reset(KillScope::Global, 0, "operator", "try clear", 2'000, true, true).error().code,
              "KILL_SWITCH_JOURNAL");
}

TEST(KillSwitchTest, CancelSinkCallingActiveAndEngineCompleteDoesNotDeadlock) {
    KillSwitch* kill_ptr = nullptr;
    RiskEngine* engine_ptr = nullptr;
    bool active_observed_in_cancel = false;
    bool active_observed_in_sink = false;

    KillSwitch kill(
        [&](const KillSwitchEvent& event) {
            if (event.active) active_observed_in_sink = kill_ptr->active(1, 5);
            return common::Result<void>::success();
        },
        [&](const KillScope, const std::uint64_t) {
            active_observed_in_cancel = kill_ptr->active(1, 5);
            engine_ptr->complete("order-1");
            return common::Result<void>::success();
        });
    RiskEngine engine(kill);
    kill_ptr = &kill;
    engine_ptr = &engine;

    const auto accepted = engine.evaluate(intent("order-1"), risk_snapshot());
    ASSERT_TRUE(accepted.accepted);
    ASSERT_EQ(engine.reservation_count(), 1U);

    auto task = std::async(std::launch::async, [&] {
        return kill.activate(KillScope::Account, 1, "operator", "lock-order check", 1'000);
    });
    ASSERT_EQ(task.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto event = task.get();
    EXPECT_TRUE(active_observed_in_cancel);
    EXPECT_TRUE(active_observed_in_sink);
    EXPECT_TRUE(event.working_orders_cancelled);
    EXPECT_TRUE(event.persisted);
    EXPECT_TRUE(kill.active(1, 5));
    EXPECT_EQ(engine.reservation_count(), 0U);
}

TEST(KillSwitchTest, ConcurrentActivationsAssignUniqueSequencesWithoutDeadlock) {
    TemporaryDirectory directory;
    persistence::EventLog log(directory.path() / "concurrent-kill.blog");
    ASSERT_TRUE(log.recover());

    KillSwitch* kill_ptr = nullptr;
    KillSwitch kill(KillSwitch::event_log_sink(log), [&](const KillScope, const std::uint64_t) {
        EXPECT_TRUE(kill_ptr->active(1));
        return common::Result<void>::success();
    });
    kill_ptr = &kill;

    constexpr std::size_t thread_count = 8;
    constexpr std::size_t calls_per_thread = 16;
    auto runner = std::async(std::launch::async, [&] {
        std::vector<std::future<std::vector<std::uint64_t>>> workers;
        workers.reserve(thread_count);
        for (std::size_t t = 0; t < thread_count; ++t) {
            workers.push_back(std::async(std::launch::async, [&, t] {
                std::vector<std::uint64_t> sequences;
                sequences.reserve(calls_per_thread);
                for (std::size_t i = 0; i < calls_per_thread; ++i) {
                    const auto ev = kill.activate(
                        KillScope::Account, 1, "worker-" + std::to_string(t),
                        "concurrent", static_cast<std::int64_t>(1'000 + t * 100 + i));
                    sequences.push_back(ev.sequence);
                }
                return sequences;
            }));
        }
        std::set<std::uint64_t> unique_sequences;
        for (auto& worker : workers) {
            for (const auto seq : worker.get()) unique_sequences.insert(seq);
        }
        return unique_sequences;
    });
    ASSERT_EQ(runner.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    const auto unique_sequences = runner.get();
    constexpr std::size_t expected_total = thread_count * calls_per_thread;
    EXPECT_EQ(unique_sequences.size(), expected_total);
    EXPECT_EQ(*unique_sequences.begin(), 1U);
    EXPECT_EQ(*unique_sequences.rbegin(), expected_total);
    EXPECT_EQ(kill.events().size(), expected_total);
}

TEST(FeeCalculatorTest, AppliesVersionedComponentsCapsAndGSTWithIntegerRounding) {
    FeeSchedule schedule;
    schedule.schedule_id = "test-schedule";
    schedule.version = 3;
    schedule.source = "local fixture";
    schedule.components.push_back({FeeComponentKind::Brokerage, 100, 50, 100, 8'000, true});
    schedule.gst_basis_points = 1'800;
    const auto buy = FeeCalculator::calculate(Side::Buy, 1'000'000, schedule);
    ASSERT_TRUE(buy);
    EXPECT_EQ(buy.value().total_micro, 9'440); // Capped 8,000 brokerage plus 18% GST.
    EXPECT_EQ(buy.value().lines.size(), 2U);
    EXPECT_EQ(buy.value().lines[1].kind, FeeComponentKind::Gst);
    const auto sell = FeeCalculator::calculate(Side::Sell, 1'000'000, schedule);
    ASSERT_TRUE(sell);
    EXPECT_LT(sell.value().total_micro, buy.value().total_micro);
    EXPECT_EQ(FeeCalculator::calculate(Side::Buy, 1'000'000, FeeSchedule{}).value().total_micro, 0);
    schedule.gst_basis_points = 10'001;
    EXPECT_EQ(FeeCalculator::calculate(Side::Buy, 1'000'000, schedule).error().code, "FEE_SCHEDULE_INVALID");
}

TEST(PaperExecutionTest, ConservativeQuoteUsesAskBidAndVisibleSizeWithAdverseSlippage) {
    PaperExecutionSimulator simulator;
    auto order = intent();
    order.quantity = 10;
    auto market = quote_market();
    auto config = execution_config(PaperExecutionModel::ConservativeQuote);
    const auto buy = simulator.execute(order, market, config);
    ASSERT_TRUE(buy);
    ASSERT_EQ(buy.value().fills.size(), 1U);
    EXPECT_EQ(buy.value().fills[0].price_micro, 10'200);
    EXPECT_EQ(buy.value().filled_quantity, 10U);
    EXPECT_EQ(buy.value().slippage_micro, 1'000);
    EXPECT_EQ(buy.value().source, common::DataSourceType::Synthetic);

    order.side = Side::Sell;
    const auto sell = simulator.execute(order, market, config);
    ASSERT_TRUE(sell);
    EXPECT_EQ(sell.value().fills[0].price_micro, 9'900);
    EXPECT_EQ(sell.value().slippage_micro, 1'000);

    order.side = Side::Buy;
    order.type = OrderType::Limit;
    order.limit_price_micro = 10'000;
    const auto not_marketable = simulator.execute(order, market, config);
    ASSERT_TRUE(not_marketable);
    EXPECT_EQ(not_marketable.value().filled_quantity, 0U);
    order.limit_price_micro = 10'100;
    const auto marketable = simulator.execute(order, market, config);
    ASSERT_TRUE(marketable);
    EXPECT_EQ(marketable.value().fills[0].price_micro, 10'100);
    EXPECT_EQ(marketable.value().slippage_micro, 0);
}

TEST(PaperExecutionTest, StaleQuoteRejectsOrLeavesOrderWorkingByExplicitPolicy) {
    PaperExecutionSimulator simulator;
    auto market = quote_market();
    market.quality = common::FeedQualityState::Stale;
    auto config = execution_config(PaperExecutionModel::ConservativeQuote);
    EXPECT_EQ(simulator.execute(intent(), market, config).error().code, "STALE_DATA");
    config.leave_unfilled_working = true;
    const auto waiting = simulator.execute(intent(), market, config);
    ASSERT_TRUE(waiting);
    EXPECT_EQ(waiting.value().status, "WORKING");
    EXPECT_EQ(waiting.value().remaining_quantity, 1U);
}

TEST(PaperExecutionTest, BarModelUsesNextOpenAndStrictLimitBreachWithVolumeCap) {
    PaperExecutionSimulator simulator;
    auto order = intent();
    order.quantity = 10;
    auto market = quote_market();
    market.event_time_ns = 200;
    market.now_ns = 210;
    market.signal_bar_close_ns = 100;
    market.next_bar_open_ns = 200;
    market.half_spread_micro = 50;
    market.next_bar = market_data::OhlcvBar{10'000, 11'000, 9'000, 10'500, 100};
    auto config = execution_config(PaperExecutionModel::BarApproximation);
    config.participation_bps = 1'000;
    const auto market_fill = simulator.execute(order, market, config);
    ASSERT_TRUE(market_fill);
    EXPECT_EQ(market_fill.value().fills[0].price_micro, 10'150);
    EXPECT_EQ(market_fill.value().filled_quantity, 10U);
    EXPECT_EQ(market_fill.value().latency_ns, 100U);
    EXPECT_EQ(market_fill.value().slippage_micro, 1'500);

    market.next_bar->volume = 3;
    order.type = OrderType::Limit;
    order.limit_price_micro = 10'000;
    config.participation_bps = 10'000;
    const auto limit_fill = simulator.execute(order, market, config);
    ASSERT_TRUE(limit_fill);
    EXPECT_EQ(limit_fill.value().fills[0].price_micro, 10'000);
    EXPECT_EQ(limit_fill.value().filled_quantity, 3U);
    EXPECT_EQ(limit_fill.value().status, "PARTIALLY_FILLED");
    market.next_bar_open_ns = market.signal_bar_close_ns;
    EXPECT_EQ(simulator.execute(order, market, config).error().code, "NEXT_BAR_UNAVAILABLE");
}

TEST(PaperExecutionTest, OrderBookReplaySweepsOnlyRecordedAvailableDepth) {
    PaperExecutionSimulator simulator;
    auto order = intent();
    order.quantity = 6;
    order.created_at_ns = 90;
    auto market = quote_market();
    market.asks = {{10'100, 2}, {10'200, 10}};
    auto config = execution_config(PaperExecutionModel::OrderBookReplay);
    config.participation_bps = 5'000;
    config.simulated_latency_ns = 20;
    const auto report = simulator.execute(order, market, config);
    ASSERT_TRUE(report);
    EXPECT_EQ(report.value().filled_quantity, 6U);
    ASSERT_EQ(report.value().fills.size(), 2U);
    EXPECT_EQ(report.value().fills[0].price_micro, 10'100);
    EXPECT_EQ(report.value().fills[0].quantity, 1U);
    EXPECT_EQ(report.value().fills[1].price_micro, 10'200);
    EXPECT_EQ(report.value().fills[1].quantity, 5U);
    EXPECT_EQ(report.value().slippage_micro, 500);

    market.sequence_gap = true;
    EXPECT_EQ(simulator.execute(order, market, config).error().code, "STALE_DATA");
}

TEST(PaperExecutionTest, InternalClobUsesOnlyAnInstrumentBoundLegacyEngine) {
    PaperExecutionSimulator simulator(7);
    basalt::MatchingEngine book;
    ASSERT_EQ(book.submit_order({700, basalt::Side::Sell, basalt::OrderType::Limit, 10'100, 5}).status,
              basalt::OrderStatus::New);
    auto order = intent();
    order.order_id = 701;
    order.quantity = 3;
    auto config = execution_config(PaperExecutionModel::InternalClob);
    const auto report = simulator.execute(order, quote_market(), config, &book);
    ASSERT_TRUE(report);
    EXPECT_EQ(report.value().filled_quantity, 3U);
    EXPECT_EQ(report.value().fills[0].price_micro, 10'100);
    EXPECT_EQ(report.value().source, common::DataSourceType::Derived);
    EXPECT_EQ(PaperExecutionSimulator(8).execute(order, quote_market(), config, &book).error().code,
              "INTERNAL_BOOK_REQUIRED");
    EXPECT_FALSE(book.book().has_order(701));
    EXPECT_TRUE(book.book().has_order(700));
}

TEST(PortfolioLedgerTest, PartialFillsFeesMarksAndShortFlipReconcileExactly) {
    PortfolioLedger ledger;
    ASSERT_TRUE(ledger.create_account({1, "paper", 10'000'000, 0, true}));
    ASSERT_TRUE(ledger.apply_fill(fill(1, 1, Side::Buy, 100, 10'000, 500)));
    ASSERT_TRUE(ledger.apply_fill(fill(2, 2, Side::Sell, 40, 12'000, 200)));
    auto position = ledger.position(1, 7);
    ASSERT_TRUE(position);
    ASSERT_TRUE(position.value());
    EXPECT_EQ(position.value()->quantity, 60);
    EXPECT_EQ(position.value()->average_cost_micro, 10'000);
    EXPECT_EQ(position.value()->realized_pnl_micro, 80'000);
    EXPECT_EQ(position.value()->fees_micro, 700);
    ASSERT_TRUE(ledger.mark(1, 7, 11'000, 11'100));
    EXPECT_EQ(ledger.position(1, 7).value()->unrealized_pnl_micro, 60'000);

    ASSERT_TRUE(ledger.apply_fill(fill(3, 3, Side::Sell, 80, 9'000, 100)));
    position = ledger.position(1, 7);
    ASSERT_TRUE(position.value());
    EXPECT_EQ(position.value()->quantity, -20);
    EXPECT_EQ(position.value()->average_cost_micro, 9'000);
    EXPECT_EQ(position.value()->realized_pnl_micro, 20'000);
    ASSERT_TRUE(ledger.mark(1, 7, 8'900, 9'500));
    EXPECT_EQ(ledger.position(1, 7).value()->mark_source, "CONSERVATIVE_ASK");
    EXPECT_EQ(ledger.position(1, 7).value()->unrealized_pnl_micro, -10'000);

    const auto account = ledger.account(1);
    ASSERT_TRUE(account);
    EXPECT_EQ(account.value().cash_balance_micro, 10'199'200);
    const auto summary = ledger.summary(1);
    ASSERT_TRUE(summary);
    EXPECT_EQ(summary.value().total_fees_micro, 800);
    EXPECT_EQ(summary.value().realized_pnl_micro, 19'200);
    EXPECT_EQ(summary.value().unrealized_pnl_micro, -10'000);
    ASSERT_TRUE(ledger.verify_reconciliation());
    ASSERT_EQ(ledger.journal().size(), 3U);
    EXPECT_EQ(ledger.journal()[0].debit_micro, 1'000'500);
    EXPECT_EQ(ledger.journal()[0].contra_credit_micro, 1'000'500);
    EXPECT_EQ(ledger.journal()[1].credit_micro, 479'800);
    EXPECT_EQ(ledger.journal()[1].contra_debit_micro, 479'800);
}

TEST(PortfolioLedgerTest, RetainsCostRemainderAcrossPartialCloseAndAssignsItAtFinalClose) {
    PortfolioLedger ledger;
    ASSERT_TRUE(ledger.create_account({1, "paper", 1'000'000, 0, false}));
    ASSERT_TRUE(ledger.apply_fill(fill(1, 1, Side::Buy, 3, 101)));
    ASSERT_TRUE(ledger.apply_fill(fill(2, 2, Side::Buy, 2, 102)));
    auto position = ledger.position(1, 7).value().value();
    EXPECT_EQ(position.average_cost_micro, 101);
    EXPECT_EQ(position.cost_residual, 2);
    ASSERT_TRUE(ledger.apply_fill(fill(3, 3, Side::Sell, 1, 100)));
    position = ledger.position(1, 7).value().value();
    EXPECT_EQ(position.quantity, 4);
    EXPECT_EQ(position.cost_residual, 2);
    EXPECT_EQ(position.realized_pnl_micro, -1);
    ASSERT_TRUE(ledger.apply_fill(fill(4, 4, Side::Sell, 4, 100)));
    position = ledger.position(1, 7).value().value();
    EXPECT_EQ(position.quantity, 0);
    EXPECT_EQ(position.cost_residual, 0);
    EXPECT_EQ(position.realized_pnl_micro, -7);
    ASSERT_TRUE(ledger.verify_reconciliation());
}

TEST(PortfolioLedgerTest, RejectsOverspendShortingDuplicatesAndArithmeticBeforeMutation) {
    PortfolioLedger ledger;
    ASSERT_TRUE(ledger.create_account({1, "cash-only", 10'000, 0, false}));
    EXPECT_EQ(ledger.apply_fill(fill(1, 1, Side::Buy, 2, 10'000)).error().code, "INSUFFICIENT_CASH");
    ASSERT_TRUE(ledger.apply_fill(fill(2, 2, Side::Buy, 1, 10'000)));
    EXPECT_EQ(ledger.apply_fill(fill(3, 3, Side::Sell, 2, 10'000)).error().code, "INSUFFICIENT_POSITION");
    EXPECT_EQ(ledger.apply_fill(fill(2, 4, Side::Sell, 1, 10'000)).error().code, "DUPLICATE_EXECUTION");
    EXPECT_EQ(ledger.apply_fill(fill(4, 2, Side::Sell, 1, 10'000)).error().code, "DUPLICATE_EXECUTION");
    EXPECT_EQ(ledger.position(1, 7).value()->quantity, 1);
    ASSERT_TRUE(ledger.verify_reconciliation());
}

TEST(PortfolioLedgerTest, MissingQuoteUsesExplicitCostBasisFallbackWithoutInventedPnl) {
    PortfolioLedger ledger;
    ASSERT_TRUE(ledger.create_account({1, "paper", 100'000, 0, true}));
    ASSERT_TRUE(ledger.apply_fill(fill(1, 1, Side::Buy, 2, 10'000)));
    ASSERT_TRUE(ledger.mark(1, 7, std::nullopt, std::nullopt));
    const auto position = ledger.position(1, 7).value().value();
    EXPECT_EQ(position.mark_micro, 10'000);
    EXPECT_EQ(position.mark_source, "AVERAGE_COST_FALLBACK_NO_QUOTE");
    EXPECT_EQ(position.unrealized_pnl_micro, 0);
}

TEST(PortfolioLedgerTest, EventLogJournalRestoresAccountsPositionsAndCashAfterRestart) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "ledger.blog";
    persistence::EventLog log(path);
    ASSERT_TRUE(log.recover());
    {
        PortfolioLedger ledger(PortfolioLedger::event_log_sink(log));
        ASSERT_TRUE(ledger.create_account({1, "durable-paper", 2'000'000, 0, true}));
        ASSERT_TRUE(ledger.apply_fill(fill(1, 50, Side::Buy, 4, 10'000, 200)));
        ASSERT_TRUE(ledger.verify_reconciliation());
    }
    persistence::EventLog reopened(path);
    ASSERT_TRUE(reopened.recover());
    const auto frames = reopened.read_all();
    ASSERT_TRUE(frames);
    PortfolioLedger restored;
    ASSERT_TRUE(restored.restore(frames.value()));
    EXPECT_EQ(restored.account(1).value().cash_balance_micro, 1'959'800);
    EXPECT_EQ(restored.position(1, 7).value()->quantity, 4);
    EXPECT_EQ(restored.position(1, 7).value()->fees_micro, 200);
    ASSERT_TRUE(restored.verify_reconciliation());
}

} // namespace
} // namespace basalt::trading
