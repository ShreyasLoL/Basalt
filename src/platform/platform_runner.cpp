#include "basalt/platform/platform_runner.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace basalt::platform {
namespace {

using namespace std::chrono_literals;

class SyntheticDemoStrategy final : public strategy::IStrategy {
public:
    [[nodiscard]] strategy::StrategyDescriptor descriptor() const override {
        return {"SYNTHETIC_DEMO", "Synthetic path demonstration", "1.0.0",
                "synthetic-demo-v1", 0, {market_data::MarketEventKind::Quote}, {}, {}};
    }

    [[nodiscard]] common::Result<void, strategy::StrategyError> configure(
        const strategy::StrategyConfig& config) override {
        if (config.order_quantity == 0) return common::Result<void, strategy::StrategyError>::failure(
            {"INVALID_STRATEGY_CONFIG", "Synthetic demo order quantity must be positive"});
        configured_ = true;
        return common::Result<void, strategy::StrategyError>::success();
    }

    [[nodiscard]] common::Result<void, strategy::StrategyError> initialise(
        const strategy::StrategyContext&) override {
        if (!configured_) return common::Result<void, strategy::StrategyError>::failure(
            {"STRATEGY_NOT_CONFIGURED", "Configure the synthetic demo before starting it"});
        return common::Result<void, strategy::StrategyError>::success();
    }

    [[nodiscard]] common::Result<std::vector<strategy::StrategySignal>, strategy::StrategyError> on_event(
        const market_data::CanonicalMarketEvent& event,
        const strategy::StrategyContext& context) override {
        std::vector<strategy::StrategySignal> signals;
        if (event.kind != market_data::MarketEventKind::Quote || !event.provider_sequence)
            return common::Result<std::vector<strategy::StrategySignal>, strategy::StrategyError>::success(
                std::move(signals));

        const std::uint64_t sequence = *event.provider_sequence;
        trading::Side side{};
        common::Quantity quantity{};
        if (sequence % 4U == 1U && context.position_quantity == 0) {
            side = trading::Side::Buy;
            quantity = context.order_quantity;
        } else if (sequence % 4U == 2U) {
            // Deliberately exceed the configured risk limit so the demo proves
            // the signal-to-risk rejection path without executing the order.
            side = trading::Side::Buy;
            quantity = context.order_quantity + 1U;
        } else if (sequence % 4U == 3U && context.position_quantity > 0) {
            side = trading::Side::Sell;
            quantity = static_cast<common::Quantity>(context.position_quantity);
        } else {
            return common::Result<std::vector<strategy::StrategySignal>, strategy::StrategyError>::success(
                std::move(signals));
        }

        const std::string signal_id = "synthetic-demo-" + std::to_string(sequence);
        trading::OrderIntent intent;
        intent.client_order_id = signal_id;
        intent.order_id = 100'000U + sequence;
        intent.account_id = context.account_id;
        intent.instrument_id = context.instrument.id;
        intent.strategy_id = context.strategy_id;
        intent.side = side;
        intent.type = trading::OrderType::Market;
        intent.quantity = quantity;
        intent.tick_size_micro = context.instrument.tick_size_micro;
        intent.lot_size = context.instrument.lot_size;
        intent.created_at_ns = event.event_time_ns;
        signals.push_back({signal_id, "Deterministic synthetic path demonstration", std::move(intent), {}});
        return common::Result<std::vector<strategy::StrategySignal>, strategy::StrategyError>::success(
            std::move(signals));
    }

    void on_execution(const trading::ExecutionReport&, const strategy::StrategyContext&) override {}
    void on_stop() noexcept override {}

private:
    bool configured_{};
};

std::unique_ptr<strategy::IStrategy> make_default_strategy() {
    return std::make_unique<SyntheticDemoStrategy>();
}

void hash_byte(std::uint64_t& hash, const std::uint8_t value) noexcept {
    hash ^= value;
    hash *= 1'099'511'628'211ULL;
}

void hash_u64(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (unsigned int index = 0; index < 8; ++index) {
        hash_byte(hash, static_cast<std::uint8_t>(value & 0xffU));
        value >>= 8U;
    }
}

void hash_text(std::uint64_t& hash, const std::string& value) noexcept {
    for (const unsigned char byte : value) hash_byte(hash, byte);
    hash_byte(hash, 0xffU);
}

std::uint64_t hash_ledger(const trading::PortfolioSummary& portfolio,
                          const std::vector<trading::CashJournalEntry>& journal,
                          const std::vector<trading::ExecutionFill>& fills) noexcept {
    std::uint64_t hash = 1'469'598'103'934'665'603ULL;
    hash_u64(hash, portfolio.account_id);
    hash_u64(hash, static_cast<std::uint64_t>(portfolio.cash_balance_micro));
    hash_u64(hash, static_cast<std::uint64_t>(portfolio.total_fees_micro));
    hash_u64(hash, static_cast<std::uint64_t>(portfolio.realized_pnl_micro));
    hash_u64(hash, static_cast<std::uint64_t>(portfolio.unrealized_pnl_micro));
    for (const auto& [instrument_id, position] : portfolio.positions) {
        hash_u64(hash, instrument_id);
        hash_u64(hash, static_cast<std::uint64_t>(position.quantity));
        hash_u64(hash, static_cast<std::uint64_t>(position.average_cost_micro));
        hash_u64(hash, static_cast<std::uint64_t>(position.mark_micro.value_or(0)));
        hash_u64(hash, static_cast<std::uint64_t>(position.realized_pnl_micro));
        hash_u64(hash, static_cast<std::uint64_t>(position.unrealized_pnl_micro));
    }
    for (const auto& entry : journal) {
        hash_u64(hash, entry.execution_id);
        hash_u64(hash, entry.event_sequence);
        hash_u64(hash, entry.account_id);
        hash_u64(hash, static_cast<std::uint64_t>(entry.debit_micro));
        hash_u64(hash, static_cast<std::uint64_t>(entry.credit_micro));
        hash_u64(hash, static_cast<std::uint64_t>(entry.contra_debit_micro));
        hash_u64(hash, static_cast<std::uint64_t>(entry.contra_credit_micro));
        hash_u64(hash, static_cast<std::uint64_t>(entry.fee_micro));
        hash_u64(hash, static_cast<std::uint64_t>(entry.balance_micro));
        hash_text(hash, entry.memo);
    }
    for (const auto& fill : fills) {
        hash_u64(hash, fill.execution_id);
        hash_u64(hash, fill.event_sequence);
        hash_u64(hash, fill.order_id);
        hash_u64(hash, fill.account_id);
        hash_u64(hash, fill.instrument_id);
        hash_u64(hash, fill.strategy_id);
        hash_u64(hash, static_cast<std::uint64_t>(fill.side));
        hash_u64(hash, static_cast<std::uint64_t>(fill.price_micro));
        hash_u64(hash, fill.quantity);
        hash_u64(hash, static_cast<std::uint64_t>(fill.fee_micro));
        hash_u64(hash, static_cast<std::uint64_t>(fill.model));
        hash_u64(hash, fill.model_version);
        hash_u64(hash, static_cast<std::uint64_t>(fill.source));
        hash_u64(hash, static_cast<std::uint64_t>(fill.quality));
        hash_text(hash, fill.fee_schedule_id);
        hash_u64(hash, fill.fee_schedule_version);
        hash_text(hash, fill.fee_schedule_source);
        hash_u64(hash, static_cast<std::uint64_t>(fill.time_ns));
    }
    return hash;
}

common::Error convert_strategy_error(const strategy::StrategyError& error) {
    return {error.code, error.message};
}

} // namespace

PlatformRunner::PlatformRunner(PlatformRunnerConfig config,
                               std::unique_ptr<strategy::IStrategy> strategy)
    : config_(std::move(config)),
      instruments_(market_data::InstrumentMaster::seeded_sim_catalog()),
      provider_(instruments_, config_.base_event_time_ns, config_.random_seed),
      pipeline_(instruments_),
      event_bus_(config_.event_queue_capacity, config_.event_queue_capacity),
      risk_engine_(kill_switch_),
      execution_simulator_(config_.instrument_id),
      strategy_runtime_(strategy ? std::move(strategy) : make_default_strategy(), risk_engine_) {
    if (config_.base_event_time_ns <= 0 || config_.instrument_id == 0 ||
        config_.account_id == 0 || config_.strategy_id == 0 || config_.initial_cash_micro <= 0 ||
        config_.max_data_age_ns == 0 || config_.event_queue_capacity == 0) {
        throw std::invalid_argument("Platform runner configuration contains a zero or invalid required value");
    }
    instrument_ = instruments_.find(config_.instrument_id);
    if (instrument_ == nullptr || instrument_->exchange != market_data::Exchange::Sim || !instrument_->active) {
        throw std::invalid_argument("Platform runner instrument must be an active SIM catalog entry");
    }
    if (config_.execution_config.tick_size_micro == 0)
        config_.execution_config.tick_size_micro = instrument_->tick_size_micro;
    if (config_.execution_config.model != trading::PaperExecutionModel::ConservativeQuote) {
        throw std::invalid_argument("Platform runner currently wires the conservative quote execution model");
    }
    config_.execution_config.model = trading::PaperExecutionModel::ConservativeQuote;
}

common::Result<void> PlatformRunner::initialize() {
    if (initialized_) return common::Result<void>::success();
    const auto account = ledger_.create_account(
        {config_.account_id, "synthetic-paper", config_.initial_cash_micro, 0, false});
    if (!account) return account;

    pipeline_.register_provider(std::string(provider_.name()),
        {common::DataSourceType::Synthetic, config_.max_data_age_ns, config_.max_data_age_ns});
    pipeline_.set_event_sinks(
        [this](market_data::CanonicalMarketEvent event) {
            return event_bus_.publish_durable(std::move(event), 0ms);
        },
        [this](market_data::CanonicalMarketEvent event) {
            return event_bus_.publish_quote(std::move(event));
        });
    provider_.set_event_sink([this](market_data::RawMarketEvent raw) {
        if (next_receive_mono_ns_ == std::numeric_limits<std::int64_t>::max()) {
            callback_error_ = common::Error{"RECEIVE_SEQUENCE_OVERFLOW", "Synthetic receive clock reached its bound"};
            return;
        }
        const auto processed = pipeline_.process(raw, next_receive_mono_ns_++, raw.event_time_ns);
        if (!processed) callback_error_ = common::Error{processed.error().code, processed.error().message};
    });

    const auto connected = provider_.connect({"SYNTHETIC", {}, {}, true, true});
    if (!connected) return common::Result<void>::failure(
        {connected.error().code, connected.error().message});
    const std::array<common::InstrumentId, 1> subscriptions{config_.instrument_id};
    const auto subscribed = provider_.subscribe(subscriptions);
    if (!subscribed) return common::Result<void>::failure(
        {subscribed.error().code, subscribed.error().message});

    auto configured = strategy_runtime_.configure(config_.strategy_config);
    if (!configured) return common::Result<void>::failure(convert_strategy_error(configured.error()));
    strategy::StrategyContext context;
    context.instrument = *instrument_;
    context.account_id = config_.account_id;
    context.strategy_id = config_.strategy_id;
    context.now_ns = config_.base_event_time_ns;
    context.max_data_age_ns = config_.max_data_age_ns;
    context.risk_limits = config_.risk_limits;
    const auto started = strategy_runtime_.start(context);
    if (!started) return common::Result<void>::failure(convert_strategy_error(started.error()));
    initialized_ = true;
    return common::Result<void>::success();
}

common::Result<trading::RiskSnapshot> PlatformRunner::make_risk_snapshot(
    const market_data::CanonicalMarketEvent& event) {
    const auto account = ledger_.account(config_.account_id);
    if (!account) return common::Result<trading::RiskSnapshot>::failure(account.error());
    const auto portfolio = ledger_.summary(config_.account_id);
    if (!portfolio) return common::Result<trading::RiskSnapshot>::failure(portfolio.error());
    const auto position = ledger_.position(config_.account_id, config_.instrument_id);
    if (!position) return common::Result<trading::RiskSnapshot>::failure(position.error());

    trading::RiskSnapshot snapshot;
    snapshot.cash_available_micro = account.value().cash_balance_micro;
    snapshot.now_ns = event.receive_time_utc_ns;
    snapshot.bid_micro = event.bid_micro;
    snapshot.ask_micro = event.ask_micro;
    snapshot.data_quality_ok = event.quality == common::FeedQualityState::Ok &&
                               !event.sequence_gap && !event.clock_skew;
    snapshot.market_open = true;
    snapshot.instrument_enabled = instrument_->active;
    snapshot.strategy_enabled = true;
    snapshot.allow_short = account.value().allow_short;
    snapshot.open_orders = risk_engine_.reservation_count();
    snapshot.strategy_orders = snapshot.open_orders;
    if (event.receive_time_utc_ns >= event.event_time_ns) {
        snapshot.data_age_ns = static_cast<std::uint64_t>(event.receive_time_utc_ns - event.event_time_ns);
    }

    std::int64_t marked_value = 0;
    if (position.value()) {
        const auto& state = *position.value();
        if (state.mark_micro) {
            const auto value = common::checked_multiply(*state.mark_micro, state.quantity);
            if (!value) return common::Result<trading::RiskSnapshot>::failure(value.error());
            marked_value = value.value();
        }
        snapshot.position_quantity = state.quantity;
    }
    const auto gross = marked_value < 0 ? common::checked_multiply(marked_value, -1)
                                        : common::Result<std::int64_t>::success(marked_value);
    if (!gross) return common::Result<trading::RiskSnapshot>::failure(gross.error());
    snapshot.gross_exposure_micro = gross.value();
    snapshot.net_exposure_micro = marked_value;
    snapshot.instrument_exposure_micro = gross.value();
    snapshot.strategy_exposure_micro = gross.value();
    const auto pnl = common::checked_add(portfolio.value().realized_pnl_micro,
                                         portfolio.value().unrealized_pnl_micro);
    if (!pnl) return common::Result<trading::RiskSnapshot>::failure(pnl.error());
    snapshot.daily_pnl_micro = pnl.value();
    snapshot.strategy_daily_pnl_micro = pnl.value();
    const auto equity = common::checked_add(account.value().cash_balance_micro, marked_value);
    if (!equity) return common::Result<trading::RiskSnapshot>::failure(equity.error());
    snapshot.equity_micro = equity.value();
    high_water_mark_micro_ = std::max(high_water_mark_micro_, equity.value());
    snapshot.high_water_mark_micro = high_water_mark_micro_;
    return common::Result<trading::RiskSnapshot>::success(std::move(snapshot));
}

common::Result<void> PlatformRunner::execute_signal(
    const strategy::StrategySignal& signal,
    const market_data::CanonicalMarketEvent& event,
    const strategy::StrategyContext& context) {
    trading::OrderIntent intent = signal.intent;
    if (kill_switch_.active(config_.account_id, config_.strategy_id)) {
        risk_engine_.complete(intent.client_order_id);
        halted_ = true;
        return common::Result<void>::success();
    }
    if (intent.order_id == 0) {
        if (next_order_id_ == std::numeric_limits<std::uint64_t>::max()) {
            risk_engine_.complete(intent.client_order_id);
            return common::Result<void>::failure({"ORDER_ID_OVERFLOW", "Platform order ID reached its bound"});
        }
        intent.order_id = next_order_id_++;
    }
    trading::ExecutionMarket market;
    market.event_sequence = event.provider_sequence.value_or(0);
    market.source = event.source;
    market.quality = event.quality;
    market.event_time_ns = event.event_time_ns;
    market.receive_time_ns = event.receive_time_utc_ns;
    market.now_ns = event.receive_time_utc_ns;
    market.sequence_gap = event.sequence_gap;
    market.bid_micro = event.bid_micro;
    market.bid_quantity = event.bid_quantity;
    market.ask_micro = event.ask_micro;
    market.ask_quantity = event.ask_quantity;

    const auto report = execution_simulator_.execute(intent, market, config_.execution_config);
    if (!report) {
        risk_engine_.complete(intent.client_order_id);
        return common::Result<void>::failure(report.error());
    }
    for (const auto& paper_fill : report.value().fills) {
        if (next_execution_id_ == std::numeric_limits<std::uint64_t>::max() ||
            next_ledger_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
            risk_engine_.complete(intent.client_order_id);
            return common::Result<void>::failure({"LEDGER_SEQUENCE_OVERFLOW", "Platform ledger sequence reached its bound"});
        }
        trading::ExecutionFill fill;
        fill.execution_id = next_execution_id_++;
        fill.event_sequence = next_ledger_sequence_++;
        fill.order_id = intent.order_id;
        fill.account_id = report.value().account_id;
        fill.instrument_id = report.value().instrument_id;
        fill.strategy_id = report.value().strategy_id;
        fill.side = report.value().side;
        fill.price_micro = paper_fill.price_micro;
        fill.quantity = paper_fill.quantity;
        fill.fee_micro = paper_fill.fee_micro;
        fill.model = report.value().model;
        fill.model_version = report.value().model_version;
        fill.source = report.value().source;
        fill.quality = report.value().quality;
        fill.fee_schedule_id = report.value().fee_schedule_id;
        fill.fee_schedule_version = report.value().fee_schedule_version;
        fill.fee_schedule_source = report.value().fee_schedule_source;
        fill.time_ns = event.event_time_ns;
        const auto applied = ledger_.apply_fill(fill);
        if (!applied) {
            risk_engine_.complete(intent.client_order_id);
            return applied;
        }
        ++fills_;
    }
    risk_engine_.complete(intent.client_order_id);
    if (report.value().filled_quantity > 0) {
        const auto marked = ledger_.mark(config_.account_id, config_.instrument_id,
                                         event.bid_micro, event.ask_micro);
        if (!marked) return marked;
    }
    auto execution_context = context;
    const auto snapshot = make_risk_snapshot(event);
    if (!snapshot) return common::Result<void>::failure(snapshot.error());
    execution_context.position_quantity = snapshot.value().position_quantity;
    execution_context.risk_snapshot = snapshot.value();
    const auto notified = strategy_runtime_.on_execution(report.value(), execution_context);
    if (!notified) return common::Result<void>::failure(convert_strategy_error(notified.error()));
    return common::Result<void>::success();
}

common::Result<void> PlatformRunner::process_event(const market_data::CanonicalMarketEvent& event) {
    if (event.kind != market_data::MarketEventKind::Quote || event.instrument_id != config_.instrument_id)
        return common::Result<void>::success();
    ++ticks_processed_;
    latest_market_event_ = event;

    const auto current_position = ledger_.position(config_.account_id, config_.instrument_id);
    if (!current_position) return common::Result<void>::failure(current_position.error());
    if (current_position.value()) {
        const auto marked = ledger_.mark(config_.account_id, config_.instrument_id,
                                         event.bid_micro, event.ask_micro);
        if (!marked) return marked;
    }

    const auto snapshot = make_risk_snapshot(event);
    if (!snapshot) return common::Result<void>::failure(snapshot.error());
    strategy::StrategyContext context;
    context.instrument = *instrument_;
    context.account_id = config_.account_id;
    context.strategy_id = config_.strategy_id;
    context.position_quantity = snapshot.value().position_quantity;
    context.now_ns = event.receive_time_utc_ns;
    context.max_data_age_ns = config_.max_data_age_ns;
    context.risk_snapshot = snapshot.value();
    context.risk_limits = config_.risk_limits;
    context.order_quantity = config_.strategy_config.order_quantity;
    context.sizing_budget_micro = config_.strategy_config.sizing_budget_micro;
    context.fee_bps = config_.strategy_config.fee_bps;
    context.slippage_bps = config_.strategy_config.slippage_bps;

    const auto dispatched = strategy_runtime_.on_market_event(event, context);
    if (!dispatched) return common::Result<void>::failure(convert_strategy_error(dispatched.error()));
    signals_ += dispatched.value().approved_signals.size() + dispatched.value().rejected_signals.size();
    for (const auto& rejected : dispatched.value().rejected_signals) {
        risk_rejection_codes_.push_back(rejected.decision.code);
        risk_rejections_.emplace_back(rejected.decision.code,
            event.provider_sequence.value_or(static_cast<std::uint64_t>(ticks_processed_)));
    }
    for (std::size_t index = 0; index < dispatched.value().approved_signals.size(); ++index) {
        if (kill_switch_.active(config_.account_id, config_.strategy_id)) {
            halted_ = true;
            for (std::size_t pending = index; pending < dispatched.value().approved_signals.size(); ++pending) {
                risk_engine_.complete(dispatched.value().approved_signals[pending].intent.client_order_id);
            }
            break;
        }
        const auto& approved = dispatched.value().approved_signals[index];
        ++orders_;
        const auto executed = execute_signal(approved, event, context);
        if (!executed) return executed;
    }
    return common::Result<void>::success();
}

common::Result<PlatformSessionSnapshot> PlatformRunner::run(const std::size_t cycles) {
    std::lock_guard lock(mutex_);
    if (cycles == 0 || cycles > 100'000) return common::Result<PlatformSessionSnapshot>::failure(
        {"INVALID_STEP_COUNT", "Platform session cycles must be between 1 and 100000"});
    const auto ready = initialize();
    if (!ready) return common::Result<PlatformSessionSnapshot>::failure(ready.error());

    for (std::size_t cycle = 0; cycle < cycles; ++cycle) {
        if (kill_switch_.active(config_.account_id, config_.strategy_id)) {
            halted_ = true;
            break;
        }
        callback_error_.reset();
        const auto stepped = provider_.step(1);
        if (!stepped) return common::Result<PlatformSessionSnapshot>::failure(
            {stepped.error().code, stepped.error().message});
        if (callback_error_) return common::Result<PlatformSessionSnapshot>::failure(*callback_error_);
        if (stepped.value() == 0) continue;
        while (auto event = event_bus_.pop_durable(0ms)) {
            static_cast<void>(event_bus_.drain_quotes());
            const auto processed = process_event(*event);
            if (!processed) return common::Result<PlatformSessionSnapshot>::failure(processed.error());
        }
    }
    return snapshot_locked();
}

common::Result<PlatformSessionSnapshot> PlatformRunner::snapshot_locked() const {
    const auto portfolio = ledger_.summary(config_.account_id);
    if (!portfolio) return common::Result<PlatformSessionSnapshot>::failure(portfolio.error());
    const auto pnl = common::checked_add(portfolio.value().realized_pnl_micro,
                                         portfolio.value().unrealized_pnl_micro);
    if (!pnl) return common::Result<PlatformSessionSnapshot>::failure(pnl.error());
    PlatformSessionSnapshot snapshot;
    snapshot.ticks_processed = ticks_processed_;
    snapshot.signals = signals_;
    snapshot.orders = orders_;
    snapshot.fills = fills_;
    snapshot.risk_rejections = risk_rejection_codes_.size();
    snapshot.risk_rejection_codes = risk_rejection_codes_;
    snapshot.risk_rejection_events = risk_rejections_;
    snapshot.total_pnl_micro = pnl.value();
    snapshot.ledger_hash = hash_ledger(portfolio.value(), ledger_.journal(), ledger_.fills());
    snapshot.execution_halted = halted_ || kill_switch_.active(config_.account_id, config_.strategy_id);
    snapshot.portfolio = portfolio.value();
    snapshot.risk_limits = config_.risk_limits;
    return common::Result<PlatformSessionSnapshot>::success(std::move(snapshot));
}

common::Result<PlatformSessionSnapshot> PlatformRunner::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_locked();
}

common::Result<trading::PortfolioSummary> PlatformRunner::portfolio(
    const common::AccountId account_id) const {
    std::lock_guard lock(mutex_);
    return ledger_.summary(account_id);
}

std::vector<PlatformMarketSnapshot> PlatformRunner::market_watch() const {
    std::lock_guard lock(mutex_);
    std::vector<PlatformMarketSnapshot> rows;
    const auto instruments = instruments_.list(market_data::Exchange::Sim, true);
    rows.reserve(instruments.size());
    for (const auto& instrument : instruments) {
        std::optional<market_data::CanonicalMarketEvent> event;
        if (latest_market_event_ && latest_market_event_->instrument_id == instrument.id)
            event = latest_market_event_;
        rows.push_back({instrument, std::move(event)});
    }
    return rows;
}

common::Result<PlatformMarketSnapshot> PlatformRunner::market_snapshot(
    const common::InstrumentId instrument_id) const {
    std::lock_guard lock(mutex_);
    const auto* instrument = instruments_.find(instrument_id);
    if (instrument == nullptr || instrument->exchange != market_data::Exchange::Sim) {
        return common::Result<PlatformMarketSnapshot>::failure(
            {"UNKNOWN_INSTRUMENT", "Instrument ID is not present in the SIM instrument catalog"});
    }
    std::optional<market_data::CanonicalMarketEvent> event;
    if (latest_market_event_ && latest_market_event_->instrument_id == instrument_id)
        event = latest_market_event_;
    return common::Result<PlatformMarketSnapshot>::success({*instrument, std::move(event)});
}

std::vector<trading::ExecutionFill> PlatformRunner::recent_fills(const std::size_t limit) const {
    std::lock_guard lock(mutex_);
    const auto& fills = ledger_.fills();
    const auto first = fills.size() > limit ? fills.size() - limit : 0;
    return {fills.begin() + static_cast<std::ptrdiff_t>(first), fills.end()};
}

PlatformTelemetrySnapshot PlatformRunner::telemetry() const {
    std::lock_guard lock(mutex_);
    return PlatformTelemetrySnapshot{pipeline_.stats(), event_bus_.stats()};
}

common::Result<void> PlatformRunner::verify_reconciliation() const {
    std::lock_guard lock(mutex_);
    return ledger_.verify_reconciliation();
}

trading::KillSwitchEvent PlatformRunner::activate_kill_switch(
    std::string actor, std::string reason) {
    // KillSwitch owns its own lock so a caller can latch the stop while run()
    // is processing a longer deterministic batch.
    return kill_switch_.activate(trading::KillScope::Global, 0, std::move(actor),
                                 std::move(reason), config_.base_event_time_ns);
}

} // namespace basalt::platform
