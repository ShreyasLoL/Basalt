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
};

common::Result<std::int64_t> absolute_quantity(const common::PositionQuantity quantity) {
    if (quantity == std::numeric_limits<common::PositionQuantity>::min())
        return common::Result<std::int64_t>::failure({"ARITHMETIC_OVERFLOW", "Position magnitude overflows"});
    return common::Result<std::int64_t>::success(quantity < 0 ? -quantity : quantity);
}

common::Result<std::int64_t> subtract(const std::int64_t left, const std::int64_t right) {
    if ((right > 0 && left < std::numeric_limits<std::int64_t>::min() + right) ||
        (right < 0 && left > std::numeric_limits<std::int64_t>::max() + right))
        return common::Result<std::int64_t>::failure({"ARITHMETIC_OVERFLOW", "Signed subtraction overflow"});
    return common::Result<std::int64_t>::success(left - right);
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
            it->filled_quantity += report.filled_quantity;
            order_record.updated_at_ns = current.open_time_ns;
            order_record.filled_quantity = it->filled_quantity;
            order_record.remaining_quantity = report.remaining_quantity;
            order_record.status = report.status;
            order_record.reason = report.reason;
            if (report.slippage_micro > 0) {
                const auto signed_cost = report.side == trading::Side::Buy
                    ? report.slippage_micro : -report.slippage_micro;
                auto next_signed = common::checked_add(result.signed_slippage_micro, signed_cost);
                auto next_absolute = common::checked_add(result.absolute_slippage_micro, report.slippage_micro);
                if (!next_signed || !next_absolute) return common::Result<BacktestResult>::failure(
                    {"BACKTEST_SLIPPAGE_OVERFLOW", "Accumulated slippage overflow"});
                result.signed_slippage_micro = next_signed.value();
                result.absolute_slippage_micro = next_absolute.value();
            }
            for (const auto& paper_fill : report.fills) {
                trading::ExecutionFill fill{};
                fill.execution_id = next_execution_id++;
                fill.event_sequence = next_ledger_sequence++;
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
                auto prior = ledger.position(config.account_id, config.instrument.id);
                if (!prior) return common::Result<BacktestResult>::failure(prior.error());
                const auto prior_state = prior.value();
                auto applied = ledger.apply_fill(fill);
                if (!applied) return common::Result<BacktestResult>::failure(applied.error());
                auto after = ledger.position(config.account_id, config.instrument.id);
                if (!after) return common::Result<BacktestResult>::failure(after.error());
                const auto prior_realized = prior_state ? prior_state->realized_pnl_micro : 0;
                const auto next_realized = after.value() ? after.value()->realized_pnl_micro : 0;
                auto realized_delta = subtract(next_realized, prior_realized);
                if (!realized_delta) return common::Result<BacktestResult>::failure(realized_delta.error());
                const auto prior_quantity = prior_state ? prior_state->quantity : 0;
                const auto after_quantity = after.value() ? after.value()->quantity : 0;
                if (prior_quantity == 0 && after_quantity != 0) {
                    run_position_opened = true;
                    run_round_trip_pnl = 0;
                } else if (prior_quantity != 0) {
                    auto accumulated = common::checked_add(run_round_trip_pnl, realized_delta.value());
                    if (!accumulated) return common::Result<BacktestResult>::failure(accumulated.error());
                    run_round_trip_pnl = accumulated.value();
                    const bool flipped = (prior_quantity < 0 && after_quantity > 0) ||
                                         (prior_quantity > 0 && after_quantity < 0);
                    if (after_quantity == 0 || flipped) {
                        if (run_position_opened) result.closed_trade_pnl_micro.push_back(run_round_trip_pnl);
                        run_round_trip_pnl = 0;
                        run_position_opened = flipped;
                    }
                }
                auto notional = common::checked_multiply(fill.price_micro, static_cast<std::int64_t>(fill.quantity));
                if (!notional) return common::Result<BacktestResult>::failure(notional.error());
                auto turnover = common::checked_add(result.turnover_micro, notional.value());
                auto fees = common::checked_add(result.total_fees_micro, fill.fee_micro);
                if (!turnover || !fees) return common::Result<BacktestResult>::failure(
                    {"BACKTEST_LEDGER_OVERFLOW", "Turnover or fee accumulation overflow"});
                result.turnover_micro = turnover.value();
                result.total_fees_micro = fees.value();
                result.fills.push_back(fill);
                ++result.execution_count;
            }
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
                               order_index, 0, 0, false, 0});
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

} // namespace basalt::backtest
