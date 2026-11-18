#include "basalt/backtest/backtest.hpp"

#include "basalt/persistence/history.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>

#ifndef BASALT_GIT_REVISION
#define BASALT_GIT_REVISION "unknown"
#endif
#ifndef BASALT_GIT_WORKTREE_DIRTY
#define BASALT_GIT_WORKTREE_DIRTY true
#endif

namespace basalt::backtest {
namespace {

using Json = nlohmann::json;

std::string compiler_metadata() {
#if defined(__clang__)
    return std::string("Clang ") + __clang_version__;
#elif defined(_MSC_VER)
    return std::string("MSVC ") + std::to_string(_MSC_VER);
#elif defined(__GNUC__)
    return std::string("GCC ") + __VERSION__;
#else
    return "Unknown C++ compiler";
#endif
}

struct PendingOrder {
    trading::OrderIntent intent{};
    std::int64_t signal_close_ns{};
    common::EventSequence signal_sequence{};
    std::size_t result_order_index{};
    std::uint64_t bars_seen{};
    common::Quantity filled_quantity{};
    bool opened_in_run{};
    std::int64_t round_trip_realized_micro{};
    std::int64_t accumulated_slippage_micro{};
};

common::Result<std::int64_t> absolute_quantity(const common::PositionQuantity quantity) {
    if (quantity >= 0) return common::Result<std::int64_t>::success(quantity);
    const auto magnitude = common::checked_subtract(0, quantity);
    if (!magnitude)
        return common::Result<std::int64_t>::failure({"ARITHMETIC_OVERFLOW", "Position magnitude overflows"});
    return magnitude;
}

common::Quantity participation(const common::Quantity quantity, const std::uint32_t bps) {
    return (quantity / 10'000U) * bps + ((quantity % 10'000U) * bps) / 10'000U;
}

bool valid_hash(const std::string& value) {
    if (value.size() != 64) return false;
    return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
               (character >= 'A' && character <= 'F');
    });
}

Json fee_schedule_json(const trading::FeeSchedule& fees) {
    Json components = Json::array();
    for (const auto& item : fees.components) {
        components.push_back({{"kind", static_cast<unsigned>(item.kind)},
                              {"buy_bps", item.buy_basis_points}, {"sell_bps", item.sell_basis_points},
                              {"fixed_micro", item.fixed_micro}, {"cap_micro", item.cap_micro},
                              {"gst_eligible", item.gst_eligible}});
    }
    return {{"id", fees.schedule_id}, {"version", fees.version}, {"source", fees.source},
            {"venue", fees.venue}, {"segment", fees.segment}, {"product", fees.product},
            {"valid_from_ns", fees.valid_from_ns}, {"valid_to_ns", fees.valid_to_ns},
            {"gst_bps", fees.gst_basis_points}, {"components", std::move(components)}};
}

Json limits_json(const trading::RiskLimits& limits) {
    return {{"max_order_notional_micro", limits.max_order_notional_micro},
            {"max_order_quantity", limits.max_order_quantity},
            {"max_position_quantity", limits.max_position_quantity},
            {"max_gross_exposure_micro", limits.max_gross_exposure_micro},
            {"max_net_exposure_micro", limits.max_net_exposure_micro},
            {"max_concentration_bps", limits.max_concentration_bps},
            {"max_daily_loss_micro", limits.max_daily_loss_micro},
            {"max_drawdown_micro", limits.max_drawdown_micro},
            {"max_open_orders", limits.max_open_orders},
            {"max_orders_per_window", limits.max_orders_per_window},
            {"order_rate_window_ns", limits.order_rate_window_ns},
            {"max_stale_age_ns", limits.max_stale_age_ns},
            {"max_instrument_position_quantity", limits.max_instrument_position_quantity},
            {"max_instrument_notional_micro", limits.max_instrument_notional_micro},
            {"max_strategy_orders", limits.max_strategy_orders},
            {"max_strategy_exposure_micro", limits.max_strategy_exposure_micro},
            {"max_strategy_daily_loss_micro", limits.max_strategy_daily_loss_micro}};
}

Json strategy_config_json(const strategy::StrategyConfig& config) {
    return {{"integer_parameters", config.integer_parameters}, {"text_parameters", config.text_parameters},
            {"order_quantity", config.order_quantity}, {"random_seed", config.random_seed},
            {"sizing_budget_micro", config.sizing_budget_micro}, {"fee_bps", config.fee_bps},
            {"slippage_bps", config.slippage_bps}};
}

std::string input_bytes(const std::vector<BacktestEvent>& events) {
    Json rows = Json::array();
    for (const auto& record : events) {
        Json row{{"sequence", record.sequence}, {"open_time_ns", record.open_time_ns},
                 {"close_time_ns", record.close_time_ns}, {"session_key", record.session_key},
                 {"schema_version", record.event.schema_version},
                 {"event_id", record.event.event_id}, {"instrument_id", record.event.instrument_id},
                 {"provider", record.event.provider}, {"event_kind", market_data::to_string(record.event.kind)},
                 {"event_time_ns", record.event.event_time_ns},
                 {"receive_time_utc_ns", record.event.receive_time_utc_ns},
                 {"receive_time_mono_ns", record.event.receive_time_mono_ns},
                 {"provider_sequence", record.event.provider_sequence
                    ? Json(*record.event.provider_sequence) : Json(nullptr)},
                 {"reconnect_generation", record.event.reconnect_generation},
                 {"provenance_label", record.event.provenance_label},
                 {"source", common::to_string(record.event.source)},
                 {"quality", common::to_string(record.event.quality)},
                 {"sequence_gap", record.event.sequence_gap}, {"clock_skew", record.event.clock_skew}};
        row["price_micro"] = record.event.price_micro ? Json(*record.event.price_micro) : Json(nullptr);
        row["price_ticks"] = record.event.price_ticks ? Json(*record.event.price_ticks) : Json(nullptr);
        row["quantity"] = record.event.quantity ? Json(*record.event.quantity) : Json(nullptr);
        row["bid_micro"] = record.event.bid_micro ? Json(*record.event.bid_micro) : Json(nullptr);
        row["bid_ticks"] = record.event.bid_ticks ? Json(*record.event.bid_ticks) : Json(nullptr);
        row["bid_quantity"] = record.event.bid_quantity ? Json(*record.event.bid_quantity) : Json(nullptr);
        row["ask_micro"] = record.event.ask_micro ? Json(*record.event.ask_micro) : Json(nullptr);
        row["ask_ticks"] = record.event.ask_ticks ? Json(*record.event.ask_ticks) : Json(nullptr);
        row["ask_quantity"] = record.event.ask_quantity ? Json(*record.event.ask_quantity) : Json(nullptr);
        row["bids"] = Json::array();
        for (const auto& level : record.event.bids)
            row["bids"].push_back({{"price_micro", level.price_micro}, {"price_ticks", level.price_ticks},
                                   {"quantity", level.quantity}});
        row["asks"] = Json::array();
        for (const auto& level : record.event.asks)
            row["asks"].push_back({{"price_micro", level.price_micro}, {"price_ticks", level.price_ticks},
                                   {"quantity", level.quantity}});
        if (record.event.bar) row["bar"] = {{"open_micro", record.event.bar->open_micro},
            {"high_micro", record.event.bar->high_micro}, {"low_micro", record.event.bar->low_micro},
            {"close_micro", record.event.bar->close_micro}, {"volume", record.event.bar->volume}};
        rows.push_back(std::move(row));
    }
    return rows.dump();
}

std::string output_bytes(const BacktestResult& result) {
    Json output{{"valid", result.valid}, {"execution_count", result.execution_count},
        {"completed_orders", result.completed_orders}, {"cancelled_orders", result.cancelled_orders},
        {"rejected_orders", result.rejected_orders}, {"turnover_micro", result.turnover_micro},
        {"total_fees_micro", result.total_fees_micro}, {"gross_pre_fee_pnl_micro", result.gross_pre_fee_pnl_micro},
        {"signed_slippage_micro", result.signed_slippage_micro},
        {"absolute_slippage_micro", result.absolute_slippage_micro},
        {"peak_gross_exposure_micro", result.peak_gross_exposure_micro},
        {"estimated_capacity_notional_micro", result.estimated_capacity_notional_micro},
        {"estimated_capacity_quantity", result.estimated_capacity_quantity},
        {"warnings", result.warnings},
        {"compounded_relative_wealth", result.analytics.compounded_relative_wealth
            ? Json(*result.analytics.compounded_relative_wealth) : Json(nullptr)},
        {"fee_to_gross_pnl_ratio", result.analytics.fee_to_gross_pnl_ratio
            ? Json(*result.analytics.fee_to_gross_pnl_ratio) : Json(nullptr)}};
    output["fills"] = Json::array();
    for (const auto& fill : result.fills) {
        output["fills"].push_back({{"execution_id", fill.execution_id}, {"event_sequence", fill.event_sequence},
            {"order_id", fill.order_id}, {"account_id", fill.account_id}, {"instrument_id", fill.instrument_id},
            {"strategy_id", fill.strategy_id}, {"side", static_cast<unsigned>(fill.side)},
            {"price_micro", fill.price_micro}, {"quantity", fill.quantity}, {"fee_micro", fill.fee_micro},
            {"model", static_cast<unsigned>(fill.model)}, {"model_version", fill.model_version},
            {"source", common::to_string(fill.source)}, {"quality", common::to_string(fill.quality)},
            {"fee_schedule_id", fill.fee_schedule_id}, {"fee_schedule_version", fill.fee_schedule_version},
            {"fee_schedule_source", fill.fee_schedule_source}, {"time_ns", fill.time_ns}});
    }
    output["cash_journal"] = Json::array();
    for (const auto& row : result.cash_journal)
        output["cash_journal"].push_back({{"execution_id", row.execution_id}, {"event_sequence", row.event_sequence},
            {"account_id", row.account_id}, {"debit_micro", row.debit_micro}, {"credit_micro", row.credit_micro},
            {"contra_debit_micro", row.contra_debit_micro}, {"contra_credit_micro", row.contra_credit_micro},
            {"fee_micro", row.fee_micro}, {"balance_micro", row.balance_micro}, {"memo", row.memo}});
    output["equity_curve"] = Json::array();
    for (const auto& point : result.equity_curve)
        output["equity_curve"].push_back({{"time_ns", point.time_ns}, {"equity_micro", point.equity_micro}});
    output["risk_rejections"] = Json::array();
    for (const auto& [code, count] : result.risk_rejections) output["risk_rejections"].push_back({code, count});
    output["closed_trade_pnl_micro"] = result.closed_trade_pnl_micro;
    output["signals"] = Json::array();
    for (const auto& signal : result.signals)
        output["signals"].push_back({{"event_sequence", signal.event_sequence}, {"event_time_ns", signal.event_time_ns},
            {"signal_id", signal.signal_id}, {"reason", signal.reason}, {"side", static_cast<unsigned>(signal.side)},
            {"quantity", signal.quantity}, {"reference_price_micro", signal.reference_price_micro},
            {"approved", signal.approved}, {"risk_code", signal.risk_code}});
    output["orders"] = Json::array();
    for (const auto& order : result.orders)
        output["orders"].push_back({{"order_id", order.order_id}, {"client_order_id", order.client_order_id},
            {"signal_sequence", order.signal_sequence}, {"created_at_ns", order.created_at_ns},
            {"updated_at_ns", order.updated_at_ns}, {"side", static_cast<unsigned>(order.side)},
            {"requested_quantity", order.requested_quantity}, {"filled_quantity", order.filled_quantity},
            {"remaining_quantity", order.remaining_quantity}, {"status", order.status}, {"reason", order.reason}});
    output["position_journal"] = Json::array();
    for (const auto& position : result.position_journal)
        output["position_journal"].push_back({{"event_sequence", position.event_sequence},
            {"time_ns", position.time_ns}, {"quantity", position.quantity},
            {"average_cost_micro", position.average_cost_micro},
            {"realized_pnl_micro", position.realized_pnl_micro},
            {"unrealized_pnl_micro", position.unrealized_pnl_micro},
            {"mark_micro", position.mark_micro ? Json(*position.mark_micro) : Json(nullptr)},
            {"mark_source", position.mark_source}});
    output["metrics"] = Json::array();
    for (const auto& metric : result.analytics.metrics)
        output["metrics"].push_back({{"name", metric.name}, {"status", metric.status},
            {"value", metric.value ? Json(*metric.value) : Json(nullptr)}, {"units", metric.units},
            {"amount_micro", metric.amount_micro ? Json(*metric.amount_micro) : Json(nullptr)}});
    return output.dump();
}

common::Result<void> validate_events(const std::vector<BacktestEvent>& events,
                                     const BacktestConfig& config) {
    if (events.empty()) return common::Result<void>::failure({"BACKTEST_EMPTY_DATA", "Backtest input is empty"});
    std::set<std::string, std::less<>> event_ids;
    common::EventSequence previous_sequence = 0;
    std::int64_t previous_close = 0;
    std::int64_t previous_event_time = 0;
    for (const auto& record : events) {
        if (record.sequence == 0 || record.sequence <= previous_sequence || record.open_time_ns <= 1 ||
            record.close_time_ns <= record.open_time_ns || record.event.event_id.empty() ||
            !event_ids.insert(record.event.event_id).second || record.event.kind != market_data::MarketEventKind::Bar ||
            !record.event.bar || record.event.instrument_id != config.instrument.id ||
            record.event.event_time_ns != record.close_time_ns || record.event.event_time_ns < previous_event_time ||
            (previous_close > 0 && record.open_time_ns < previous_close))
            return common::Result<void>::failure({"BACKTEST_EVENT_ORDER", "Bars must be unique, chronological, non-overlapping and instrument-bound"});
        if (record.event.quality != common::FeedQualityState::Ok || record.event.sequence_gap || record.event.clock_skew)
            return common::Result<void>::failure({"BACKTEST_DATA_QUALITY", "Required input contains stale, degraded, gapped or clock-skewed market data"});
        const auto& bar = *record.event.bar;
        if (bar.open_micro <= 0 || bar.high_micro < bar.open_micro || bar.high_micro < bar.close_micro ||
            bar.low_micro <= 0 || bar.low_micro > bar.open_micro || bar.low_micro > bar.close_micro ||
            bar.volume == 0 || bar.open_micro % config.instrument.tick_size_micro != 0 ||
            bar.high_micro % config.instrument.tick_size_micro != 0 ||
            bar.low_micro % config.instrument.tick_size_micro != 0 ||
            bar.close_micro % config.instrument.tick_size_micro != 0)
            return common::Result<void>::failure({"BACKTEST_BAR_INVALID", "OHLCV bar values are inconsistent or empty"});
        if (config.bar_interval_ns == 0 || config.bar_interval_ns - 1 !=
            static_cast<std::uint64_t>(record.close_time_ns - record.open_time_ns))
            return common::Result<void>::failure({"BACKTEST_BAR_INTERVAL", "Bar timestamps must follow the declared inclusive close-timestamp convention"});
        previous_sequence = record.sequence;
        previous_close = record.close_time_ns;
        previous_event_time = record.event.event_time_ns;
    }
    return common::Result<void>::success();
}

Json manifest_json(const BacktestConfig& config, const strategy::StrategyDescriptor& descriptor,
                   const strategy::StrategyConfig& strategy_config, const std::vector<BacktestEvent>& events,
                   const std::string& input_hash, const std::string& output_hash,
                   const bool working_tree_dirty) {
    Json initial_positions = Json::array();
    for (const auto& position : config.initial_positions)
        initial_positions.push_back({{"instrument_id", position.instrument_id}, {"quantity", position.quantity},
                                     {"average_cost_micro", position.average_cost_micro}});
    Json parameters = Json::array();
    for (const auto& item : descriptor.parameter_schema)
        parameters.push_back({{"name", item.name}, {"type", static_cast<unsigned>(item.type)},
                              {"minimum", item.minimum}, {"maximum", item.maximum},
                              {"default", item.default_value}});
    const auto& begin = events.front();
    const auto& end = events.back();
    return {{"basalt_revision", config.basalt_revision}, {"working_tree_dirty", working_tree_dirty},
            {"schema_version", config.schema_version},
            {"software_version", config.software_version}, {"compiler", config.compiler},
            {"build_flags", config.build_flags}, {"strategy", {{"id", descriptor.strategy_id},
                {"version", descriptor.version}, {"source_hash", descriptor.stable_hash},
                {"numeric_id", config.strategy_id}, {"config", strategy_config_json(strategy_config)},
                {"config_hash", persistence::HistoryStore::sha256(strategy_config_json(strategy_config).dump())},
                {"parameter_schema", std::move(parameters)}}},
            {"dataset", {{"id", config.dataset_id}, {"sha256", config.dataset_sha256},
                {"input_event_sha256", input_hash}, {"source", config.data_source},
                {"license_tag", config.license_tag}, {"corporate_action_policy", config.corporate_action_policy},
                {"survivorship_safe_history", config.survivorship_safe_history}}},
            {"instrument", {{"master_version", config.instrument_master_version},
                {"instrument_id", config.instrument.id}, {"exchange", market_data::to_string(config.instrument.exchange)},
                {"segment", config.instrument.segment}, {"symbol", config.instrument.symbol},
                {"tick_size_micro", config.instrument.tick_size_micro}, {"lot_size", config.instrument.lot_size}}},
            {"event_range", {{"first_sequence", begin.sequence}, {"last_sequence", end.sequence},
                {"first_open_ns", begin.open_time_ns}, {"last_close_ns", end.close_time_ns},
                {"bar_interval_ns", config.bar_interval_ns}}},
            {"timezone", config.timezone}, {"exchange_calendar", config.exchange_calendar},
            {"seed", config.seed}, {"initial_cash_micro", config.initial_cash_micro},
            {"initial_positions", std::move(initial_positions)}, {"allow_short", config.allow_short},
            {"fee_schedule", fee_schedule_json(config.execution.fee_schedule)},
            {"execution", {{"model", trading::to_string(config.execution.model)},
                {"model_version", config.execution.model_version}, {"slippage_ticks", config.execution.slippage_ticks},
                {"participation_bps", config.execution.participation_bps},
                {"latency_ns", config.execution.simulated_latency_ns},
                {"half_spread_micro", config.half_spread_micro},
                {"max_working_bars", config.max_working_bars}}},
            {"risk_limits", limits_json(config.risk_limits)},
            {"split", {{"selected", to_string(config.selected_split)}, {"embargo_ns", config.splits.embargo_ns},
                {"train", {config.splits.train.start_ns, config.splits.train.end_ns}},
                {"validation", {config.splits.validation.start_ns, config.splits.validation.end_ns}},
                {"test", {config.splits.test.start_ns, config.splits.test.end_ns}}}},
            {"numeric_rounding_policy", config.rounding_policy},
            {"analytics_assumptions", {{"benchmark_cumulative_return", config.benchmark_cumulative_return
                    ? Json(*config.benchmark_cumulative_return) : Json(nullptr)},
                {"risk_free_return_per_period", config.risk_free_return_per_period},
                {"target_return_per_period", config.target_return_per_period},
                {"periods_per_year", config.periods_per_year}}},
            {"result_hashes", {{"output_sha256", output_hash}, {"input_events_sha256", input_hash}}}};
}

} // namespace

common::Result<BacktestResult> BacktestEngine::run(
    std::unique_ptr<strategy::IStrategy> strategy_instance,
    const strategy::StrategyConfig& supplied_strategy_config,
    const std::vector<BacktestEvent>& events, const BacktestConfig& config) const {
    if (!strategy_instance || config.schema_version == 0 || config.dataset_id.empty() ||
        !valid_hash(config.dataset_sha256) || config.data_source.empty() || config.license_tag.empty() ||
        config.instrument_master_version == 0 || config.instrument.id == 0 || !config.instrument.active ||
        config.instrument.tick_size_micro <= 0 || config.instrument.lot_size == 0 || config.account_id == 0 ||
        config.strategy_id == 0 || config.initial_cash_micro <= 0 || config.timezone.empty() ||
        config.exchange_calendar.empty() || config.bar_interval_ns == 0 || config.max_working_bars == 0 ||
        config.execution.model != trading::PaperExecutionModel::BarApproximation ||
        config.execution.model_version == 0 || config.execution.participation_bps > 10'000 ||
        config.execution.tick_size_micro != config.instrument.tick_size_micro || config.half_spread_micro < 0 ||
        config.half_spread_micro % config.instrument.tick_size_micro != 0 ||
        config.periods_per_year == 0 || config.initial_positions.size() > 1 ||
        config.risk_limits.max_order_notional_micro <= 0 || config.risk_limits.max_order_quantity == 0 ||
        config.risk_limits.max_position_quantity <= 0 || config.risk_limits.max_gross_exposure_micro <= 0 ||
        config.risk_limits.max_instrument_position_quantity <= 0 ||
        config.risk_limits.max_instrument_notional_micro <= 0 || config.risk_limits.max_strategy_exposure_micro <= 0)
        return common::Result<BacktestResult>::failure({"BACKTEST_CONFIG_INVALID", "Required backtest metadata or execution configuration is invalid"});
    auto split_check = validate_splits(config.splits);
    if (!split_check) return common::Result<BacktestResult>::failure(split_check.error());
    auto event_check = validate_events(events, config);
    if (!event_check) return common::Result<BacktestResult>::failure(event_check.error());

    const auto range = config.selected_split == DataSplit::Train ? config.splits.train :
        (config.selected_split == DataSplit::Validation ? config.splits.validation : config.splits.test);
    std::vector<BacktestEvent> selected;
    for (const auto& item : events)
        if (item.close_time_ns >= range.start_ns && item.close_time_ns < range.end_ns) selected.push_back(item);
    if (selected.empty()) return common::Result<BacktestResult>::failure(
        {"BACKTEST_SPLIT_EMPTY", "Selected dataset split contains no completed bars"});

    for (const auto& initial : config.initial_positions) {
        if (initial.instrument_id != config.instrument.id || initial.quantity == 0 || initial.average_cost_micro <= 0 ||
            initial.average_cost_micro % config.instrument.tick_size_micro != 0 ||
            (initial.quantity < 0 && !config.allow_short))
            return common::Result<BacktestResult>::failure(
                {"BACKTEST_INITIAL_POSITION", "Initial position is invalid, off tick or shorting is disabled"});
    }

    auto strategy_config = supplied_strategy_config;
    strategy_config.random_seed = config.seed;
    const auto descriptor = strategy_instance->descriptor();
    if (descriptor.strategy_id.empty() || descriptor.stable_hash.empty() || descriptor.version.empty())
        return common::Result<BacktestResult>::failure({"BACKTEST_STRATEGY_INVALID", "Strategy descriptor is incomplete"});
    if (descriptor.strategy_id == "VWAP_REVERSION" &&
        std::any_of(selected.begin(), selected.end(), [](const auto& item) { return item.session_key.empty(); }))
        return common::Result<BacktestResult>::failure(
            {"BACKTEST_SESSION_REQUIRED", "VWAP backtests require exchange-calendar session keys for each bar"});

    trading::KillSwitch kill_switch;
    trading::RiskEngine risk_engine(kill_switch);
    strategy::StrategyRuntime runtime(std::move(strategy_instance), risk_engine);
    auto configured = runtime.configure(strategy_config);
    if (!configured) return common::Result<BacktestResult>::failure({configured.error().code, configured.error().message});

    BacktestResult result{};
    result.input_event_sha256 = persistence::HistoryStore::sha256(input_bytes(events));
    if (config.execution.fee_schedule.components.empty() &&
        config.execution.fee_schedule.gst_basis_points == 0)
        result.warnings.push_back("fees: none (cost-free)");
    if (!config.survivorship_safe_history)
        result.warnings.push_back("SURVIVORSHIP_SAFE_HISTORY_NOT_VERIFIED");
    if (config.corporate_action_policy.empty() || config.corporate_action_policy == "unspecified")
        result.warnings.push_back("CORPORATE_ACTION_POLICY_UNSPECIFIED");
    if (config.license_tag == "unspecified") result.warnings.push_back("DATA_LICENSE_UNSPECIFIED");

    trading::PortfolioLedger ledger;
    auto account_created = ledger.create_account({config.account_id, "BASALT_BACKTEST", config.initial_cash_micro,
                                                   config.initial_cash_micro, config.allow_short});
    if (!account_created) return common::Result<BacktestResult>::failure(account_created.error());
    std::uint64_t next_execution_id = 1;
    std::uint64_t next_ledger_sequence = 1;
    for (const auto& initial : config.initial_positions) {
        const auto magnitude = absolute_quantity(initial.quantity);
        if (!magnitude) return common::Result<BacktestResult>::failure(magnitude.error());
        trading::ExecutionFill seed{};
        seed.execution_id = next_execution_id++;
        seed.order_id = seed.execution_id;
        seed.event_sequence = next_ledger_sequence++;
        seed.account_id = config.account_id;
        seed.instrument_id = initial.instrument_id;
        seed.strategy_id = config.strategy_id;
        seed.side = initial.quantity > 0 ? trading::Side::Buy : trading::Side::Sell;
        seed.price_micro = initial.average_cost_micro;
        seed.quantity = static_cast<common::Quantity>(magnitude.value());
        seed.model = trading::PaperExecutionModel::BarApproximation;
        seed.source = common::DataSourceType::Historical;
        seed.quality = common::FeedQualityState::Ok;
        seed.fee_schedule_id = "INITIAL_POSITION";
        seed.fee_schedule_source = "backtest_manifest";
        seed.time_ns = selected.front().open_time_ns - 1;
        auto applied = ledger.apply_fill(seed);
        if (!applied) return common::Result<BacktestResult>::failure(applied.error());
        result.fills.push_back(seed);
    }

    strategy::StrategyContext context{};
    context.instrument = config.instrument;
    context.account_id = config.account_id;
    context.strategy_id = config.strategy_id;
    context.now_ns = selected.front().open_time_ns;
    context.max_data_age_ns = config.execution.stale_after_ns;
    context.allow_short = config.allow_short;
    context.risk_limits = config.risk_limits;
    auto started = runtime.start(context);
    if (!started) return common::Result<BacktestResult>::failure({started.error().code, started.error().message});
    struct RuntimeStopGuard {
        strategy::StrategyRuntime& runtime;
        ~RuntimeStopGuard() { (void)runtime.stop(); }
    };
    [[maybe_unused]] RuntimeStopGuard stop_guard{runtime};

    std::vector<PendingOrder> pending;
    std::uint64_t next_backtest_order_id = next_execution_id;
    std::map<std::string, std::uint64_t, std::less<>> rejection_counts;
    std::int64_t run_round_trip_pnl = 0;
    bool run_position_opened = false;
    std::int64_t high_water = config.initial_cash_micro;
    trading::PaperExecutionSimulator simulator;
    auto execution_config = config.execution;
    execution_config.leave_unfilled_working = true;

    auto mark_state = [&](const BacktestEvent& event) -> common::Result<void> {
        const auto close = event.event.bar->close_micro;
        auto existing_position = ledger.position(config.account_id, config.instrument.id);
        if (!existing_position) return common::Result<void>::failure(existing_position.error());
        if (existing_position.value()) {
            auto marked = ledger.mark(config.account_id, config.instrument.id, close, close);
            if (!marked) return marked;
        }
        auto account = ledger.account(config.account_id);
        auto position = ledger.position(config.account_id, config.instrument.id);
        if (!account) return common::Result<void>::failure(account.error());
        if (!position) return common::Result<void>::failure(position.error());
        common::PositionQuantity quantity = position.value() ? position.value()->quantity : 0;
        auto signed_value = common::checked_multiply(quantity, close);
        if (!signed_value) return common::Result<void>::failure(signed_value.error());
        auto equity = common::checked_add(account.value().cash_balance_micro, signed_value.value());
        if (!equity || equity.value() <= 0) return common::Result<void>::failure(
            {"BACKTEST_EQUITY_INVALID", "Marked equity is non-positive or overflowed"});
        high_water = std::max(high_water, equity.value());
        auto absolute = absolute_quantity(quantity);
        if (!absolute) return common::Result<void>::failure(absolute.error());
        auto gross = common::checked_multiply(absolute.value(), close);
        if (!gross) return common::Result<void>::failure(gross.error());
        result.peak_gross_exposure_micro = std::max(result.peak_gross_exposure_micro, gross.value());
        result.equity_curve.push_back({event.close_time_ns, equity.value()});
        result.exposure_curve.emplace_back(event.close_time_ns, gross.value());
        context.position_quantity = quantity;
        context.risk_snapshot.position_quantity = quantity;
        context.risk_snapshot.cash_available_micro = std::max<std::int64_t>(0, account.value().cash_balance_micro);
        context.risk_snapshot.cash_reserved_micro = 0;
        context.risk_snapshot.gross_exposure_micro = gross.value();
        context.risk_snapshot.net_exposure_micro = signed_value.value();
        context.risk_snapshot.instrument_exposure_micro = gross.value();
        context.risk_snapshot.strategy_exposure_micro = gross.value();
        context.risk_snapshot.equity_micro = equity.value();
        context.risk_snapshot.high_water_mark_micro = high_water;
        context.risk_snapshot.daily_pnl_micro = equity.value() - config.initial_cash_micro;
        context.risk_snapshot.strategy_daily_pnl_micro = context.risk_snapshot.daily_pnl_micro;
        context.risk_snapshot.data_age_ns = 0;
        context.risk_snapshot.now_ns = event.close_time_ns;
        context.risk_snapshot.bid_micro = close;
        context.risk_snapshot.ask_micro = close;
        context.risk_snapshot.open_orders = pending.size();
        context.risk_snapshot.strategy_orders = pending.size();
        context.risk_snapshot.data_quality_ok = event.event.quality == common::FeedQualityState::Ok;
        context.risk_snapshot.allow_short = config.allow_short;
        context.now_ns = event.close_time_ns;
        context.session_key = event.session_key;
        const auto state = position.value();
        result.position_journal.push_back({event.sequence, event.close_time_ns, quantity,
            state ? state->average_cost_micro : 0, state ? state->realized_pnl_micro : 0,
            state ? state->unrealized_pnl_micro : 0, state ? state->mark_micro : std::nullopt,
            "HISTORICAL_BAR_CLOSE_AS_MARK"});
        return common::Result<void>::success();
    };

    const auto initial_position = config.initial_positions.empty() ? common::PositionQuantity{0}
                                                                    : config.initial_positions.front().quantity;
    result.equity_curve.push_back({selected.front().open_time_ns, config.initial_cash_micro});
    auto initial_abs = absolute_quantity(initial_position);
    if (!initial_abs) return common::Result<BacktestResult>::failure(initial_abs.error());
    auto initial_exposure = common::checked_multiply(initial_abs.value(),
        config.initial_positions.empty() ? 0 : config.initial_positions.front().average_cost_micro);
    if (!initial_exposure) return common::Result<BacktestResult>::failure(initial_exposure.error());
    result.exposure_curve.push_back({selected.front().open_time_ns, initial_exposure.value()});
    result.peak_gross_exposure_micro = initial_exposure.value();
    high_water = std::max(high_water, config.initial_cash_micro);

    const auto initial_seed_fill_count = result.fills.size();

    auto apply_fill_to_state = [&](trading::PortfolioLedger& target_ledger,
                                   const trading::ExecutionFill& fill,
                                   std::int64_t& round_trip_pnl,
                                   bool& position_opened,
                                   std::vector<std::int64_t>& closed_trades,
                                   std::int64_t& turnover_micro,
                                   std::int64_t& total_fees_micro,
                                   std::uint64_t& execution_count) -> common::Result<void> {
        auto prior = target_ledger.position(config.account_id, config.instrument.id);
        if (!prior) return common::Result<void>::failure(prior.error());
        const auto prior_state = prior.value();
        auto applied = target_ledger.apply_fill(fill);
        if (!applied) return applied;
        auto after = target_ledger.position(config.account_id, config.instrument.id);
        if (!after) return common::Result<void>::failure(after.error());
        const auto prior_realized = prior_state ? prior_state->realized_pnl_micro : 0;
        const auto next_realized = after.value() ? after.value()->realized_pnl_micro : 0;
        auto realized_delta = common::checked_subtract(next_realized, prior_realized);
        if (!realized_delta) return common::Result<void>::failure(realized_delta.error());
        const auto prior_quantity = prior_state ? prior_state->quantity : 0;
        const auto after_quantity = after.value() ? after.value()->quantity : 0;
        if (prior_quantity == 0 && after_quantity != 0) {
            position_opened = true;
            round_trip_pnl = 0;
        } else if (prior_quantity != 0) {
            auto accumulated = common::checked_add(round_trip_pnl, realized_delta.value());
            if (!accumulated) return common::Result<void>::failure(accumulated.error());
            round_trip_pnl = accumulated.value();
            const bool flipped = (prior_quantity < 0 && after_quantity > 0) ||
                                 (prior_quantity > 0 && after_quantity < 0);
            if (after_quantity == 0 || flipped) {
                if (position_opened) closed_trades.push_back(round_trip_pnl);
                round_trip_pnl = 0;
                position_opened = flipped;
            }
        }
        auto notional = common::checked_multiply(fill.price_micro, static_cast<std::int64_t>(fill.quantity));
        if (!notional) return common::Result<void>::failure(notional.error());
        auto turnover = common::checked_add(turnover_micro, notional.value());
        auto fees = common::checked_add(total_fees_micro, fill.fee_micro);
        if (!turnover || !fees) return common::Result<void>::failure(
            {"BACKTEST_LEDGER_OVERFLOW", "Turnover or fee accumulation overflow"});
        turnover_micro = turnover.value();
        total_fees_micro = fees.value();
        ++execution_count;
        return common::Result<void>::success();
    };

    auto rollback_order_fills = [&](const std::uint64_t rejected_order_id) -> common::Result<void> {
        trading::PortfolioLedger rebuilt;
        auto created = rebuilt.create_account({config.account_id, "BASALT_BACKTEST", config.initial_cash_micro,
                                               config.initial_cash_micro, config.allow_short});
        if (!created) return created;
        std::vector<trading::ExecutionFill> kept_fills;
        kept_fills.reserve(result.fills.size());
        std::uint64_t exec_id = 1;
        std::uint64_t seq_id = 1;
        std::int64_t rebuilt_round_trip = 0;
        bool rebuilt_opened = false;
        std::vector<std::int64_t> rebuilt_closed_trades;
        std::int64_t rebuilt_turnover = 0;
        std::int64_t rebuilt_fees = 0;
        std::uint64_t rebuilt_exec_count = 0;
        for (std::size_t idx = 0; idx < result.fills.size(); ++idx) {
            auto fill = result.fills[idx];
            if (idx >= initial_seed_fill_count && fill.order_id == rejected_order_id) continue;
            fill.execution_id = exec_id++;
            fill.event_sequence = seq_id++;
            if (idx < initial_seed_fill_count) {
                auto seeded = rebuilt.apply_fill(fill);
                if (!seeded) return seeded;
            } else {
                auto reapplied = apply_fill_to_state(rebuilt, fill, rebuilt_round_trip, rebuilt_opened,
                                                     rebuilt_closed_trades, rebuilt_turnover,
                                                     rebuilt_fees, rebuilt_exec_count);
                if (!reapplied) return reapplied;
            }
            kept_fills.push_back(std::move(fill));
        }
        ledger = std::move(rebuilt);
        result.fills = std::move(kept_fills);
        next_execution_id = exec_id;
        next_ledger_sequence = seq_id;
        run_round_trip_pnl = rebuilt_round_trip;
        run_position_opened = rebuilt_opened;
        result.closed_trade_pnl_micro = std::move(rebuilt_closed_trades);
        result.turnover_micro = rebuilt_turnover;
        result.total_fees_micro = rebuilt_fees;
        result.execution_count = rebuilt_exec_count;
        return common::Result<void>::success();
    };

    for (const auto& current : selected) {
        // Orders created from bar t close are first eligible at a strictly later bar open.
        for (auto it = pending.begin(); it != pending.end();) {
            ++it->bars_seen;
            if (current.open_time_ns <= it->signal_close_ns || current.sequence <= it->signal_sequence) {
                ++it;
                continue;
            }
            const auto& fee_schedule = execution_config.fee_schedule;
            if ((fee_schedule.valid_from_ns > 0 && current.open_time_ns < fee_schedule.valid_from_ns) ||
                (fee_schedule.valid_to_ns > 0 && current.open_time_ns >= fee_schedule.valid_to_ns))
                return common::Result<BacktestResult>::failure(
                    {"FEE_SCHEDULE_OUT_OF_RANGE", "Selected fee schedule is not effective at this simulated fill time"});
            trading::ExecutionMarket market{};
            market.event_sequence = current.sequence;
            market.source = current.event.source;
            market.quality = current.event.quality;
            market.event_time_ns = current.close_time_ns;
            market.receive_time_ns = current.close_time_ns;
            market.now_ns = current.close_time_ns;
            market.sequence_gap = current.event.sequence_gap;
            market.next_bar = current.event.bar;
            market.signal_bar_close_ns = it->signal_close_ns;
            market.next_bar_open_ns = current.open_time_ns;
            market.half_spread_micro = config.half_spread_micro;
            const auto execution = simulator.execute(it->intent, market, execution_config);
            if (!execution) return common::Result<BacktestResult>::failure(execution.error());
            const auto& report = execution.value();
            auto& order_record = result.orders[it->result_order_index];
            if (report.filled_quantity > std::numeric_limits<common::Quantity>::max() - it->filled_quantity)
                return common::Result<BacktestResult>::failure(
                    {"BACKTEST_FILL_OVERFLOW", "Order filled quantity overflow"});

            // Stage fills on a copy so a fill-time cash or position shortfall rolls back
            // atomically per order without aborting the backtest run.
            auto staged_ledger = ledger;
            auto staged_exec_id = next_execution_id;
            auto staged_seq_id = next_ledger_sequence;
            auto staged_round_trip = run_round_trip_pnl;
            auto staged_opened = run_position_opened;
            auto staged_closed_trades = result.closed_trade_pnl_micro;
            auto staged_turnover = result.turnover_micro;
            auto staged_fees = result.total_fees_micro;
            auto staged_exec_count = result.execution_count;
            std::vector<trading::ExecutionFill> staged_fills;
            staged_fills.reserve(report.fills.size());
            std::string fill_time_rejection_code;

            for (const auto& paper_fill : report.fills) {
                trading::ExecutionFill fill{};
                fill.execution_id = staged_exec_id++;
                fill.event_sequence = staged_seq_id++;
                fill.order_id = it->intent.order_id;
                fill.account_id = it->intent.account_id;
                fill.instrument_id = it->intent.instrument_id;
                fill.strategy_id = it->intent.strategy_id;
                fill.side = it->intent.side;
                fill.price_micro = paper_fill.price_micro;
                fill.quantity = paper_fill.quantity;
                fill.fee_micro = paper_fill.fee_micro;
                fill.model = report.model;
                fill.model_version = report.model_version;
                fill.source = report.source;
                fill.quality = report.quality;
                fill.fee_schedule_id = report.fee_schedule_id;
                fill.fee_schedule_version = report.fee_schedule_version;
                fill.fee_schedule_source = report.fee_schedule_source;
                fill.time_ns = current.open_time_ns;
                auto applied = apply_fill_to_state(staged_ledger, fill, staged_round_trip, staged_opened,
                                                   staged_closed_trades, staged_turnover,
                                                   staged_fees, staged_exec_count);
                if (!applied) {
                    if (applied.error().code == "INSUFFICIENT_CASH") {
                        fill_time_rejection_code = "FILL_TIME_INSUFFICIENT_CASH";
                        break;
                    }
                    if (applied.error().code == "INSUFFICIENT_POSITION") {
                        fill_time_rejection_code = "FILL_TIME_INSUFFICIENT_POSITION";
                        break;
                    }
                    return common::Result<BacktestResult>::failure(applied.error());
                }
                staged_fills.push_back(std::move(fill));
            }

            if (!fill_time_rejection_code.empty()) {
                if (it->filled_quantity > 0) {
                    if (it->accumulated_slippage_micro > 0) {
                        const auto signed_cost = it->intent.side == trading::Side::Buy
                            ? it->accumulated_slippage_micro : -it->accumulated_slippage_micro;
                        result.signed_slippage_micro -= signed_cost;
                        result.absolute_slippage_micro -= it->accumulated_slippage_micro;
                    }
                    auto rolled_back = rollback_order_fills(it->intent.order_id);
                    if (!rolled_back) return common::Result<BacktestResult>::failure(rolled_back.error());
                }
                order_record.updated_at_ns = current.open_time_ns;
                order_record.filled_quantity = 0;
                order_record.remaining_quantity = order_record.requested_quantity;
                order_record.status = "REJECTED";
                order_record.reason = fill_time_rejection_code;
                ++result.rejected_orders;
                ++rejection_counts[fill_time_rejection_code];
                risk_engine.complete(it->intent.client_order_id);
                it = pending.erase(it);
                continue;
            }

            if (report.slippage_micro > 0) {
                const auto signed_cost = report.side == trading::Side::Buy
                    ? report.slippage_micro : -report.slippage_micro;
                auto next_signed = common::checked_add(result.signed_slippage_micro, signed_cost);
                auto next_absolute = common::checked_add(result.absolute_slippage_micro, report.slippage_micro);
                auto next_order_slip = common::checked_add(it->accumulated_slippage_micro, report.slippage_micro);
                if (!next_signed || !next_absolute || !next_order_slip)
                    return common::Result<BacktestResult>::failure(
                        {"BACKTEST_SLIPPAGE_OVERFLOW", "Accumulated slippage overflow"});
                result.signed_slippage_micro = next_signed.value();
                result.absolute_slippage_micro = next_absolute.value();
                it->accumulated_slippage_micro = next_order_slip.value();
            }
            ledger = std::move(staged_ledger);
            next_execution_id = staged_exec_id;
            next_ledger_sequence = staged_seq_id;
            run_round_trip_pnl = staged_round_trip;
            run_position_opened = staged_opened;
            result.closed_trade_pnl_micro = std::move(staged_closed_trades);
            result.turnover_micro = staged_turnover;
            result.total_fees_micro = staged_fees;
            result.execution_count = staged_exec_count;
            for (auto& fill : staged_fills) result.fills.push_back(std::move(fill));

            it->filled_quantity += report.filled_quantity;
            order_record.updated_at_ns = current.open_time_ns;
            order_record.filled_quantity = it->filled_quantity;
            order_record.remaining_quantity = report.remaining_quantity;
            order_record.status = report.status;
            order_record.reason = report.reason;
            if (report.status == "FILLED" || report.remaining_quantity == 0) {
                ++result.completed_orders;
                risk_engine.complete(it->intent.client_order_id);
                it = pending.erase(it);
            } else if (report.filled_quantity == 0 && report.status != "WORKING") {
                ++result.cancelled_orders;
                order_record.status = "CANCELLED";
                risk_engine.complete(it->intent.client_order_id);
                it = pending.erase(it);
            } else if (it->bars_seen >= config.max_working_bars) {
                ++result.cancelled_orders;
                order_record.status = "CANCELLED";
                order_record.reason = "MAX_WORKING_BARS";
                risk_engine.complete(it->intent.client_order_id);
                it = pending.erase(it);
            } else {
                it->intent.quantity = report.remaining_quantity;
                ++it;
            }
        }

        auto capacity = participation(current.event.bar->volume, config.execution.participation_bps);
        capacity = std::min(capacity, config.risk_limits.max_order_quantity);
        capacity = std::min(capacity, static_cast<common::Quantity>(config.risk_limits.max_position_quantity));
        capacity = std::min(capacity, static_cast<common::Quantity>(config.risk_limits.max_instrument_position_quantity));
        capacity = std::min(capacity, static_cast<common::Quantity>(config.risk_limits.max_order_notional_micro /
                                                                      current.event.bar->close_micro));
        capacity = std::min(capacity, static_cast<common::Quantity>(config.risk_limits.max_instrument_notional_micro /
                                                                      current.event.bar->close_micro));
        capacity = std::min(capacity, static_cast<common::Quantity>(config.risk_limits.max_gross_exposure_micro /
                                                                      current.event.bar->close_micro));
        capacity = std::min(capacity, static_cast<common::Quantity>(config.risk_limits.max_strategy_exposure_micro /
                                                                      current.event.bar->close_micro));
        if (capacity > std::numeric_limits<std::uint64_t>::max() - result.estimated_capacity_quantity)
            return common::Result<BacktestResult>::failure({"BACKTEST_CAPACITY_OVERFLOW", "Capacity quantity overflow"});
        result.estimated_capacity_quantity += capacity;
        auto capacity_value = common::checked_multiply(current.event.bar->close_micro,
            static_cast<std::int64_t>(std::min<common::Quantity>(capacity,
                static_cast<common::Quantity>(std::numeric_limits<std::int64_t>::max()))));
        if (!capacity_value) return common::Result<BacktestResult>::failure(capacity_value.error());
        auto next_capacity = common::checked_add(result.estimated_capacity_notional_micro, capacity_value.value());
        if (!next_capacity) return common::Result<BacktestResult>::failure(next_capacity.error());
        result.estimated_capacity_notional_micro = next_capacity.value();

        auto marked = mark_state(current);
        if (!marked) return common::Result<BacktestResult>::failure(marked.error());
        auto dispatched = runtime.on_market_event(current.event, context);
        if (!dispatched) return common::Result<BacktestResult>::failure(
            {dispatched.error().code, dispatched.error().message});
        for (const auto& rejected : dispatched.value().rejected_signals) {
            ++rejection_counts[rejected.decision.code];
            ++result.rejected_orders;
            result.signals.push_back({current.sequence, current.close_time_ns, rejected.signal.signal_id,
                rejected.signal.reason, rejected.signal.intent.side, rejected.signal.intent.quantity,
                rejected.decision.reference_price_micro, false, rejected.decision.code});
            result.orders.push_back({0, rejected.signal.signal_id, current.sequence, current.close_time_ns,
                current.close_time_ns, rejected.signal.intent.side, rejected.signal.intent.quantity, 0,
                rejected.signal.intent.quantity, "REJECTED", rejected.decision.code});
        }
        for (auto& signal : dispatched.value().approved_signals) {
            signal.intent.order_id = next_backtest_order_id++;
            const auto reference_price = signal.intent.side == trading::Side::Buy
                ? context.risk_snapshot.ask_micro.value_or(0) : context.risk_snapshot.bid_micro.value_or(0);
            result.signals.push_back({current.sequence, current.close_time_ns, signal.signal_id,
                signal.reason, signal.intent.side, signal.intent.quantity, reference_price, true, "ACCEPTED"});
            const auto order_index = result.orders.size();
            result.orders.push_back({signal.intent.order_id, signal.signal_id, current.sequence,
                current.close_time_ns, current.close_time_ns, signal.intent.side, signal.intent.quantity,
                0, signal.intent.quantity, "PENDING", "RISK_APPROVED"});
            pending.push_back({std::move(signal.intent), current.close_time_ns, current.sequence,
                               order_index, 0, 0, false, 0, 0});
        }
    }

    for (const auto& [code, count] : rejection_counts) result.risk_rejections.emplace_back(code, count);
    for (const auto& order : pending) {
        ++result.cancelled_orders;
        risk_engine.complete(order.intent.client_order_id);
        auto& record = result.orders[order.result_order_index];
        record.status = "CANCELLED";
        record.reason = "END_OF_DATA";
        if (!selected.empty()) record.updated_at_ns = selected.back().close_time_ns;
    }
    pending.clear();
    (void)runtime.stop();
    const auto reconciled = ledger.verify_reconciliation();
    if (!reconciled) return common::Result<BacktestResult>::failure(reconciled.error());
    result.cash_journal = ledger.journal();
    result.fills = ledger.fills();
    auto final_position = ledger.position(config.account_id, config.instrument.id);
    if (!final_position) return common::Result<BacktestResult>::failure(final_position.error());
    result.gross_pre_fee_pnl_micro = final_position.value() ? final_position.value()->realized_pnl_micro : 0;
    AnalyticsInput analytics{};
    analytics.equity_curve = result.equity_curve;
    analytics.exposure_curve = result.exposure_curve;
    analytics.closed_trade_pnl_micro = result.closed_trade_pnl_micro;
    analytics.execution_count = result.execution_count;
    analytics.turnover_micro = result.turnover_micro;
    analytics.total_fees_micro = result.total_fees_micro;
    analytics.gross_pre_fee_pnl_micro = result.gross_pre_fee_pnl_micro;
    analytics.signed_slippage_micro = result.signed_slippage_micro;
    analytics.absolute_slippage_micro = result.absolute_slippage_micro;
    analytics.peak_gross_exposure_micro = result.peak_gross_exposure_micro;
    analytics.estimated_capacity_notional_micro = result.estimated_capacity_notional_micro;
    analytics.estimated_capacity_quantity = result.estimated_capacity_quantity;
    analytics.benchmark_cumulative_return = config.benchmark_cumulative_return;
    analytics.risk_free_return_per_period = config.risk_free_return_per_period;
    analytics.target_return_per_period = config.target_return_per_period;
    analytics.periods_per_year = config.periods_per_year;
    result.analytics = calculate_metrics(analytics);
    if (result.analytics.metrics.size() != 18) return common::Result<BacktestResult>::failure(
        {"BACKTEST_METRICS_INVARIANT", "Analytics engine did not produce exactly eighteen metrics"});
    result.valid = true;
    result.output_sha256 = persistence::HistoryStore::sha256(output_bytes(result));
    auto manifest_config = config;
    if (manifest_config.basalt_revision == "unknown") manifest_config.basalt_revision = BASALT_GIT_REVISION;
    if (manifest_config.compiler == "unknown") manifest_config.compiler = compiler_metadata();
    result.manifest_json = manifest_json(manifest_config, descriptor, strategy_config, events,
                                         result.input_event_sha256, result.output_sha256,
                                         BASALT_GIT_WORKTREE_DIRTY).dump();
    result.manifest_sha256 = persistence::HistoryStore::sha256(result.manifest_json);
    return common::Result<BacktestResult>::success(std::move(result));
}

common::Result<PortfolioBacktestResult> PortfolioBacktestEngine::run(
    std::vector<PortfolioBacktestLeg> legs, const PortfolioBacktestConfig& config) const {
    if (legs.empty() || config.account_id == 0 || config.initial_cash_micro <= 0 ||
        config.risk_limits.max_gross_exposure_micro <= 0 ||
        config.risk_limits.max_net_exposure_micro <= 0 ||
        config.risk_limits.max_position_quantity <= 0)
        return common::Result<PortfolioBacktestResult>::failure(
            {"PORTFOLIO_BACKTEST_CONFIG", "Portfolio backtest requires legs, positive cash and valid portfolio risk limits"});

    struct PreparedLeg {
        PortfolioBacktestLeg input{};
        std::vector<BacktestEvent> selected{};
        std::unique_ptr<strategy::StrategyRuntime> runtime{};
        strategy::StrategyContext context{};
    };
    struct TimelineItem { std::size_t leg_index{}; std::size_t event_index{}; };
    std::vector<PreparedLeg> prepared;
    prepared.reserve(legs.size());
    struct PortfolioStopGuard {
        std::vector<PreparedLeg>& legs;
        ~PortfolioStopGuard() {
            for (auto& leg : legs) if (leg.runtime) (void)leg.runtime->stop();
        }
    } stop_guard{prepared};
    std::set<common::InstrumentId> instrument_ids;
    std::map<common::InstrumentId, common::StrategyId> strategy_by_instrument;
    std::vector<TimelineItem> timeline;
    trading::KillSwitch kill_switch;
    trading::RiskEngine risk_engine(kill_switch);
    PortfolioBacktestResult result{};
    auto add_warning = [&](std::string warning) {
        if (std::find(result.warnings.begin(), result.warnings.end(), warning) == result.warnings.end())
            result.warnings.push_back(std::move(warning));
    };
    bool cost_free = true;
    std::vector<common::InstrumentId> fee_free_instruments;
    for (auto& leg : legs) {
        const auto& value = leg.config;
        if (!leg.strategy || value.schema_version == 0 || value.dataset_id.empty() ||
            !valid_hash(value.dataset_sha256) || value.data_source.empty() || value.license_tag.empty() ||
            value.timezone.empty() || value.exchange_calendar.empty() || value.periods_per_year == 0 ||
            value.instrument_master_version == 0 || value.instrument.id == 0 || !value.instrument.active ||
            value.instrument.tick_size_micro <= 0 || value.instrument.lot_size == 0 ||
            value.strategy_id == 0 || value.account_id != config.account_id ||
            value.initial_cash_micro != config.initial_cash_micro || value.allow_short != config.allow_short ||
            value.max_working_bars == 0 || value.execution.model != trading::PaperExecutionModel::BarApproximation ||
            value.execution.model_version == 0 || value.execution.participation_bps > 10'000 ||
            value.execution.tick_size_micro != value.instrument.tick_size_micro || value.half_spread_micro < 0 ||
            value.half_spread_micro % value.instrument.tick_size_micro != 0 ||
            value.risk_limits.max_order_notional_micro <= 0 || value.risk_limits.max_order_quantity == 0 ||
            value.risk_limits.max_position_quantity <= 0 || value.risk_limits.max_gross_exposure_micro <= 0 ||
            value.risk_limits.max_net_exposure_micro <= 0 || value.risk_limits.max_concentration_bps > 10'000 ||
            value.risk_limits.max_instrument_position_quantity <= 0 ||
            value.risk_limits.max_instrument_notional_micro <= 0 ||
            !instrument_ids.insert(value.instrument.id).second)
            return common::Result<PortfolioBacktestResult>::failure(
                {"PORTFOLIO_BACKTEST_LEG", "Each leg needs unique instrument metadata and shared account settings"});
        auto split_check = validate_splits(value.splits);
        if (!split_check) return common::Result<PortfolioBacktestResult>::failure(split_check.error());
        auto event_check = validate_events(leg.events, value);
        if (!event_check) return common::Result<PortfolioBacktestResult>::failure(event_check.error());
        const auto range = value.selected_split == DataSplit::Train ? value.splits.train :
            (value.selected_split == DataSplit::Validation ? value.splits.validation : value.splits.test);
        PreparedLeg item{};
        item.input = std::move(leg);
        const auto& prepared_config = item.input.config;
        for (std::size_t event_index = 0; event_index < item.input.events.size(); ++event_index) {
            const auto& event = item.input.events[event_index];
            if (event.close_time_ns >= range.start_ns && event.close_time_ns < range.end_ns) {
                item.selected.push_back(event);
            }
        }
        if (item.selected.empty()) return common::Result<PortfolioBacktestResult>::failure(
            {"BACKTEST_SPLIT_EMPTY", "A portfolio leg has no completed bars in the selected data split"});
        for (const auto& initial : item.input.config.initial_positions) {
            if (initial.instrument_id != prepared_config.instrument.id || initial.quantity == 0 ||
                initial.average_cost_micro <= 0 || initial.average_cost_micro % prepared_config.instrument.tick_size_micro != 0 ||
                (initial.quantity < 0 && !config.allow_short))
                return common::Result<PortfolioBacktestResult>::failure(
                    {"BACKTEST_INITIAL_POSITION", "A portfolio leg has an invalid initial position"});
        }
        item.input.strategy_config.random_seed = prepared_config.seed;
        const auto descriptor = item.input.strategy->descriptor();
        if (descriptor.strategy_id.empty() || descriptor.stable_hash.empty() || descriptor.version.empty())
            return common::Result<PortfolioBacktestResult>::failure(
                {"BACKTEST_STRATEGY_INVALID", "Portfolio leg strategy descriptor is incomplete"});
        if (descriptor.strategy_id == "VWAP_REVERSION" &&
            std::any_of(item.selected.begin(), item.selected.end(), [](const auto& event) { return event.session_key.empty(); }))
            return common::Result<PortfolioBacktestResult>::failure(
                {"BACKTEST_SESSION_REQUIRED", "VWAP backtests require exchange-calendar session keys for each bar"});
        item.context.instrument = prepared_config.instrument;
        item.context.account_id = config.account_id;
        item.context.strategy_id = prepared_config.strategy_id;
        item.context.now_ns = item.selected.front().open_time_ns;
        item.context.max_data_age_ns = prepared_config.execution.stale_after_ns;
        item.context.allow_short = config.allow_short;
        item.context.risk_limits = prepared_config.risk_limits;
        item.context.risk_limits.max_gross_exposure_micro = std::min(
            item.context.risk_limits.max_gross_exposure_micro, config.risk_limits.max_gross_exposure_micro);
        item.context.risk_limits.max_net_exposure_micro = std::min(
            item.context.risk_limits.max_net_exposure_micro, config.risk_limits.max_net_exposure_micro);
        item.context.risk_limits.max_position_quantity = std::min(
            item.context.risk_limits.max_position_quantity, config.risk_limits.max_position_quantity);
        item.runtime = std::make_unique<strategy::StrategyRuntime>(std::move(item.input.strategy), risk_engine);
        auto strategy_configured = item.runtime->configure(item.input.strategy_config);
        if (!strategy_configured) return common::Result<PortfolioBacktestResult>::failure(
            {strategy_configured.error().code, strategy_configured.error().message});
        auto started = item.runtime->start(item.context);
        if (!started) return common::Result<PortfolioBacktestResult>::failure(
            {started.error().code, started.error().message});
        strategy_by_instrument[prepared_config.instrument.id] = prepared_config.strategy_id;
        const bool leg_cost_free = prepared_config.execution.fee_schedule.components.empty() &&
            prepared_config.execution.fee_schedule.gst_basis_points == 0;
        cost_free = cost_free && leg_cost_free;
        if (leg_cost_free) fee_free_instruments.push_back(prepared_config.instrument.id);
        if (!prepared_config.survivorship_safe_history) add_warning("SURVIVORSHIP_SAFE_HISTORY_NOT_VERIFIED");
        if (prepared_config.corporate_action_policy.empty() || prepared_config.corporate_action_policy == "unspecified")
            add_warning("CORPORATE_ACTION_POLICY_UNSPECIFIED");
        if (prepared_config.license_tag == "unspecified") add_warning("DATA_LICENSE_UNSPECIFIED");
        prepared.push_back(std::move(item));
        for (std::size_t selected_index = 0; selected_index < prepared.back().selected.size(); ++selected_index) {
            timeline.push_back({prepared.size() - 1U, selected_index});
        }
    }

    // Build the timeline from the already filtered leg copies. It is globally ordered by
    // close time, with instrument ID as a stable tie-breaker for simultaneous bars.
    timeline.clear();
    for (std::size_t leg_index = 0; leg_index < prepared.size(); ++leg_index)
        for (std::size_t event_index = 0; event_index < prepared[leg_index].selected.size(); ++event_index)
            timeline.push_back({leg_index, event_index});
    std::sort(timeline.begin(), timeline.end(), [&](const auto& left, const auto& right) {
        const auto& a = prepared[left.leg_index].selected[left.event_index];
        const auto& b = prepared[right.leg_index].selected[right.event_index];
        if (a.close_time_ns != b.close_time_ns) return a.close_time_ns < b.close_time_ns;
        if (a.event.instrument_id != b.event.instrument_id) return a.event.instrument_id < b.event.instrument_id;
        return a.sequence < b.sequence;
    });

    struct PortfolioPendingOrder {
        trading::OrderIntent intent{};
        std::int64_t signal_close_ns{};
        common::EventSequence signal_sequence{};
        std::size_t leg_index{};
        std::uint64_t bars_seen{};
    };
    trading::PortfolioLedger ledger;
    auto created = ledger.create_account({config.account_id, "BASALT_PORTFOLIO_BACKTEST",
        config.initial_cash_micro, config.initial_cash_micro, config.allow_short});
    if (!created) return common::Result<PortfolioBacktestResult>::failure(created.error());
    auto run_start_ns = prepared.front().selected.front().open_time_ns;
    for (const auto& leg : prepared)
        run_start_ns = std::min(run_start_ns, leg.selected.front().open_time_ns);
    std::uint64_t next_execution_id = 1;
    std::uint64_t next_ledger_sequence = 1;
    for (auto& leg : prepared) {
        for (const auto& initial : leg.input.config.initial_positions) {
            const auto magnitude = absolute_quantity(initial.quantity);
            if (!magnitude) return common::Result<PortfolioBacktestResult>::failure(magnitude.error());
            trading::ExecutionFill seed{};
            seed.execution_id = next_execution_id++;
            seed.order_id = seed.execution_id;
            seed.event_sequence = next_ledger_sequence++;
            seed.account_id = config.account_id;
            seed.instrument_id = initial.instrument_id;
            seed.strategy_id = leg.input.config.strategy_id;
            seed.side = initial.quantity > 0 ? trading::Side::Buy : trading::Side::Sell;
            seed.price_micro = initial.average_cost_micro;
            seed.quantity = static_cast<common::Quantity>(magnitude.value());
            seed.model = trading::PaperExecutionModel::BarApproximation;
            seed.source = common::DataSourceType::Historical;
            seed.quality = common::FeedQualityState::Ok;
            seed.fee_schedule_id = "INITIAL_POSITION";
            seed.fee_schedule_source = "portfolio_backtest_manifest";
            seed.time_ns = run_start_ns - 1;
            auto applied = ledger.apply_fill(seed);
            if (!applied) return common::Result<PortfolioBacktestResult>::failure(applied.error());
        }
    }

    if (cost_free) add_warning("fees: none (cost-free)");
    else if (!fee_free_instruments.empty()) {
        std::ostringstream warning;
        warning << "fees: none (cost-free) for portfolio instrument(s)";
        for (const auto id : fee_free_instruments) warning << ' ' << id;
        add_warning(warning.str());
    }
    std::map<common::InstrumentId, std::int64_t> marks;
    std::map<common::InstrumentId, PortfolioInstrumentStats> instrument_stats;
    for (const auto& leg : prepared)
        instrument_stats.emplace(leg.input.config.instrument.id,
            PortfolioInstrumentStats{leg.input.config.instrument.id, 0, 0});
    std::vector<PortfolioPendingOrder> pending;
    std::map<std::string, std::uint64_t, std::less<>> rejection_counts;
    std::uint64_t next_order_id = next_execution_id;
    std::int64_t high_water = config.initial_cash_micro;

    auto risk_snapshot = [&](const common::InstrumentId instrument_id, const BacktestEvent& current)
        -> common::Result<trading::RiskSnapshot> {
        auto account = ledger.account(config.account_id);
        auto portfolio = ledger.summary(config.account_id);
        auto position = ledger.position(config.account_id, instrument_id);
        if (!account) return common::Result<trading::RiskSnapshot>::failure(account.error());
        if (!portfolio) return common::Result<trading::RiskSnapshot>::failure(portfolio.error());
        if (!position) return common::Result<trading::RiskSnapshot>::failure(position.error());
        std::int64_t gross = 0;
        std::int64_t net = 0;
        std::int64_t instrument_gross = 0;
        std::map<common::StrategyId, std::int64_t> strategy_gross;
        for (const auto& [id, state] : portfolio.value().positions) {
            const auto mark = marks.contains(id) ? marks.at(id) : state.average_cost_micro;
            auto signed_value = common::checked_multiply(state.quantity, mark);
            auto quantity_abs = absolute_quantity(state.quantity);
            if (!signed_value || !quantity_abs)
                return common::Result<trading::RiskSnapshot>::failure(
                    {"BACKTEST_EQUITY_OVERFLOW", "Portfolio exposure calculation overflowed"});
            auto gross_value = common::checked_multiply(quantity_abs.value(), mark);
            if (!gross_value) return common::Result<trading::RiskSnapshot>::failure(gross_value.error());
            auto next_gross = common::checked_add(gross, gross_value.value());
            auto next_net = common::checked_add(net, signed_value.value());
            if (!next_gross || !next_net) return common::Result<trading::RiskSnapshot>::failure(
                {"BACKTEST_EQUITY_OVERFLOW", "Portfolio exposure accumulation overflowed"});
            gross = next_gross.value();
            net = next_net.value();
            if (id == instrument_id) instrument_gross = gross_value.value();
            const auto owner = strategy_by_instrument.find(id);
            if (owner != strategy_by_instrument.end()) {
                auto strategy_total = common::checked_add(strategy_gross[owner->second], gross_value.value());
                if (!strategy_total) return common::Result<trading::RiskSnapshot>::failure(strategy_total.error());
                strategy_gross[owner->second] = strategy_total.value();
            }
        }
        auto equity = common::checked_add(account.value().cash_balance_micro, net);
        if (!equity || equity.value() <= 0) return common::Result<trading::RiskSnapshot>::failure(
            {"BACKTEST_EQUITY_INVALID", "Portfolio marked equity is non-positive or overflowed"});
        high_water = std::max(high_water, equity.value());
        const auto leg_strategy = strategy_by_instrument.at(instrument_id);
        auto daily_pnl = common::checked_subtract(equity.value(), config.initial_cash_micro);
        if (!daily_pnl) return common::Result<trading::RiskSnapshot>::failure(daily_pnl.error());
        trading::RiskSnapshot snapshot{};
        snapshot.position_quantity = position.value() ? position.value()->quantity : 0;
        snapshot.gross_exposure_micro = gross;
        snapshot.net_exposure_micro = net;
        snapshot.instrument_exposure_micro = instrument_gross;
        snapshot.strategy_exposure_micro = strategy_gross[leg_strategy];
        snapshot.daily_pnl_micro = daily_pnl.value();
        snapshot.strategy_daily_pnl_micro = daily_pnl.value();
        snapshot.equity_micro = equity.value();
        snapshot.high_water_mark_micro = high_water;
        snapshot.cash_available_micro = std::max<std::int64_t>(0, account.value().cash_balance_micro);
        snapshot.data_age_ns = 0;
        snapshot.now_ns = current.close_time_ns;
        snapshot.bid_micro = current.event.bar->close_micro;
        snapshot.ask_micro = current.event.bar->close_micro;
        snapshot.data_quality_ok = current.event.quality == common::FeedQualityState::Ok;
        snapshot.allow_short = config.allow_short;
        return common::Result<trading::RiskSnapshot>::success(std::move(snapshot));
    };

    trading::PaperExecutionSimulator simulator;
    for (const auto& scheduled : timeline) {
        auto& leg = prepared[scheduled.leg_index];
        const auto& current = leg.selected[scheduled.event_index];
        const auto instrument_id = leg.input.config.instrument.id;
        const auto& leg_config = leg.input.config;

        // Fill only against a later bar for this same instrument.
        for (auto it = pending.begin(); it != pending.end();) {
            if (it->leg_index != scheduled.leg_index) { ++it; continue; }
            ++it->bars_seen;
            if (current.open_time_ns <= it->signal_close_ns || current.sequence <= it->signal_sequence) {
                ++it;
                continue;
            }
            auto& exec_config = leg.input.config.execution;
            const auto& fees = exec_config.fee_schedule;
            if ((fees.valid_from_ns > 0 && current.open_time_ns < fees.valid_from_ns) ||
                (fees.valid_to_ns > 0 && current.open_time_ns >= fees.valid_to_ns))
                return common::Result<PortfolioBacktestResult>::failure(
                    {"FEE_SCHEDULE_OUT_OF_RANGE", "Selected fee schedule is not effective at this simulated fill time"});
            trading::ExecutionMarket market{};
            market.event_sequence = current.sequence;
            market.source = current.event.source;
            market.quality = current.event.quality;
            market.event_time_ns = current.close_time_ns;
            market.receive_time_ns = current.close_time_ns;
            market.now_ns = current.close_time_ns;
            market.sequence_gap = current.event.sequence_gap;
            market.next_bar = current.event.bar;
            market.signal_bar_close_ns = it->signal_close_ns;
            market.next_bar_open_ns = current.open_time_ns;
            market.half_spread_micro = leg_config.half_spread_micro;
            exec_config.leave_unfilled_working = true;
            auto execution = simulator.execute(it->intent, market, exec_config);
            if (!execution) return common::Result<PortfolioBacktestResult>::failure(execution.error());
            const auto& report = execution.value();
            auto staged_ledger = ledger;
            auto staged_execution_id = next_execution_id;
            auto staged_ledger_sequence = next_ledger_sequence;
            std::vector<trading::ExecutionFill> staged_fills;
            std::string rejection_code;
            for (const auto& paper_fill : report.fills) {
                trading::ExecutionFill fill{};
                fill.execution_id = staged_execution_id++;
                fill.event_sequence = staged_ledger_sequence++;
                fill.order_id = it->intent.order_id;
                fill.account_id = config.account_id;
                fill.instrument_id = it->intent.instrument_id;
                fill.strategy_id = it->intent.strategy_id;
                fill.side = it->intent.side;
                fill.price_micro = paper_fill.price_micro;
                fill.quantity = paper_fill.quantity;
                fill.fee_micro = paper_fill.fee_micro;
                fill.model = report.model;
                fill.model_version = report.model_version;
                fill.source = report.source;
                fill.quality = report.quality;
                fill.fee_schedule_id = report.fee_schedule_id;
                fill.fee_schedule_version = report.fee_schedule_version;
                fill.fee_schedule_source = report.fee_schedule_source;
                fill.time_ns = current.open_time_ns;
                auto applied = staged_ledger.apply_fill(fill);
                if (!applied) {
                    if (applied.error().code == "INSUFFICIENT_CASH") rejection_code = "FILL_TIME_INSUFFICIENT_CASH";
                    else if (applied.error().code == "INSUFFICIENT_POSITION") rejection_code = "FILL_TIME_INSUFFICIENT_POSITION";
                    else return common::Result<PortfolioBacktestResult>::failure(applied.error());
                    break;
                }
                staged_fills.push_back(std::move(fill));
            }
            if (!rejection_code.empty()) {
                ++rejection_counts[rejection_code];
                ++instrument_stats[instrument_id].risk_rejections;
                risk_engine.complete(it->intent.client_order_id);
                it = pending.erase(it);
                continue;
            }
            ledger = std::move(staged_ledger);
            next_execution_id = staged_execution_id;
            next_ledger_sequence = staged_ledger_sequence;
            for (auto& fill : staged_fills) {
                ++instrument_stats[instrument_id].executions;
                (void)fill;
            }
            auto callback_context = leg.context;
            auto updated_snapshot = risk_snapshot(instrument_id, current);
            if (!updated_snapshot) return common::Result<PortfolioBacktestResult>::failure(updated_snapshot.error());
            callback_context.risk_snapshot = updated_snapshot.value();
            const auto callback = leg.runtime->on_execution(report, callback_context);
            if (!callback) return common::Result<PortfolioBacktestResult>::failure(
                {callback.error().code, callback.error().message});
            if (report.status == "FILLED" || report.remaining_quantity == 0 ||
                (report.filled_quantity == 0 && report.status != "WORKING") ||
                it->bars_seen >= leg_config.max_working_bars) {
                risk_engine.complete(it->intent.client_order_id);
                it = pending.erase(it);
            } else {
                it->intent.quantity = report.remaining_quantity;
                ++it;
            }
        }

        marks[instrument_id] = current.event.bar->close_micro;
        auto existing = ledger.position(config.account_id, instrument_id);
        if (!existing) return common::Result<PortfolioBacktestResult>::failure(existing.error());
        if (existing.value()) {
            auto marked = ledger.mark(config.account_id, instrument_id,
                current.event.bar->close_micro, current.event.bar->close_micro);
            if (!marked) return common::Result<PortfolioBacktestResult>::failure(marked.error());
        }
        auto snapshot = risk_snapshot(instrument_id, current);
        if (!snapshot) return common::Result<PortfolioBacktestResult>::failure(snapshot.error());
        leg.context.position_quantity = snapshot.value().position_quantity;
        leg.context.risk_snapshot = snapshot.value();
        leg.context.now_ns = current.close_time_ns;
        leg.context.session_key = current.session_key;
        auto equity_point = snapshot.value().equity_micro;
        result.equity_curve.push_back({current.close_time_ns, equity_point});

        auto dispatched = leg.runtime->on_market_event(current.event, leg.context);
        if (!dispatched) return common::Result<PortfolioBacktestResult>::failure(
            {dispatched.error().code, dispatched.error().message});
        for (const auto& rejected : dispatched.value().rejected_signals) {
            ++rejection_counts[rejected.decision.code];
            ++instrument_stats[instrument_id].risk_rejections;
        }
        for (auto& signal : dispatched.value().approved_signals) {
            signal.intent.order_id = next_order_id++;
            pending.push_back({std::move(signal.intent), current.close_time_ns, current.sequence,
                               scheduled.leg_index, 0});
        }
    }

    for (const auto& order : pending) risk_engine.complete(order.intent.client_order_id);
    for (auto& leg : prepared) (void)leg.runtime->stop();
    const auto reconciled = ledger.verify_reconciliation();
    if (!reconciled) return common::Result<PortfolioBacktestResult>::failure(reconciled.error());
    auto summary = ledger.summary(config.account_id);
    if (!summary) return common::Result<PortfolioBacktestResult>::failure(summary.error());
    result.final_portfolio = summary.value();
    result.cash_journal = ledger.journal();
    result.fills = ledger.fills();
    result.instruments.reserve(instrument_stats.size());
    for (const auto& [unused, stats] : instrument_stats) {
        (void)unused;
        result.instruments.push_back(stats);
    }
    for (const auto& [code, count] : rejection_counts) result.risk_rejections.emplace_back(code, count);

    Json ledger_json{{"account_id", config.account_id}, {"cash_balance_micro", result.final_portfolio.cash_balance_micro},
                     {"total_fees_micro", result.final_portfolio.total_fees_micro},
                     {"realized_pnl_micro", result.final_portfolio.realized_pnl_micro},
                     {"unrealized_pnl_micro", result.final_portfolio.unrealized_pnl_micro}};
    ledger_json["positions"] = Json::array();
    for (const auto& [instrument_id, position] : result.final_portfolio.positions)
        ledger_json["positions"].push_back({{"instrument_id", instrument_id}, {"quantity", position.quantity},
            {"average_cost_micro", position.average_cost_micro}, {"realized_pnl_micro", position.realized_pnl_micro},
            {"unrealized_pnl_micro", position.unrealized_pnl_micro},
            {"mark_micro", position.mark_micro ? Json(*position.mark_micro) : Json(nullptr)}});
    ledger_json["fills"] = Json::array();
    for (const auto& fill : result.fills)
        ledger_json["fills"].push_back({{"execution_id", fill.execution_id}, {"event_sequence", fill.event_sequence},
            {"order_id", fill.order_id}, {"instrument_id", fill.instrument_id}, {"strategy_id", fill.strategy_id},
            {"side", static_cast<unsigned>(fill.side)}, {"price_micro", fill.price_micro},
            {"quantity", fill.quantity}, {"fee_micro", fill.fee_micro}, {"time_ns", fill.time_ns}});
    ledger_json["cash_journal"] = Json::array();
    for (const auto& entry : result.cash_journal)
        ledger_json["cash_journal"].push_back({{"execution_id", entry.execution_id},
            {"event_sequence", entry.event_sequence}, {"debit_micro", entry.debit_micro},
            {"credit_micro", entry.credit_micro}, {"fee_micro", entry.fee_micro},
            {"balance_micro", entry.balance_micro}, {"memo", entry.memo}});
    result.ledger_sha256 = persistence::HistoryStore::sha256(ledger_json.dump());
    result.valid = true;
    return common::Result<PortfolioBacktestResult>::success(std::move(result));
}

} // namespace basalt::backtest
