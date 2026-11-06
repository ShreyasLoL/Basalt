#include "basalt/trading/ledger.hpp"

#include "basalt/trading/execution.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>

namespace basalt::trading {
namespace {

using Json = nlohmann::json;

std::string_view side_name(const Side side) { return side == Side::Buy ? "BUY" : "SELL"; }

common::Result<Side> parse_side(const std::string_view text) {
    if (text == "BUY") return common::Result<Side>::success(Side::Buy);
    if (text == "SELL") return common::Result<Side>::success(Side::Sell);
    return common::Result<Side>::failure({"LEDGER_EVENT", "Unknown persisted execution side"});
}

common::Result<PaperExecutionModel> parse_model(const std::string_view text) {
    if (text == "CONSERVATIVE_QUOTE") return common::Result<PaperExecutionModel>::success(PaperExecutionModel::ConservativeQuote);
    if (text == "BAR_APPROXIMATION") return common::Result<PaperExecutionModel>::success(PaperExecutionModel::BarApproximation);
    if (text == "ORDER_BOOK_REPLAY") return common::Result<PaperExecutionModel>::success(PaperExecutionModel::OrderBookReplay);
    if (text == "INTERNAL_CLOB") return common::Result<PaperExecutionModel>::success(PaperExecutionModel::InternalClob);
    return common::Result<PaperExecutionModel>::failure({"LEDGER_EVENT", "Unknown persisted paper execution model"});
}

common::Result<common::DataSourceType> parse_source(const std::string_view text) {
    if (text == "LIVE_PROVIDER") return common::Result<common::DataSourceType>::success(common::DataSourceType::LiveProvider);
    if (text == "DELAYED_PROVIDER") return common::Result<common::DataSourceType>::success(common::DataSourceType::DelayedProvider);
    if (text == "HISTORICAL") return common::Result<common::DataSourceType>::success(common::DataSourceType::Historical);
    if (text == "CACHED") return common::Result<common::DataSourceType>::success(common::DataSourceType::Cached);
    if (text == "SYNTHETIC") return common::Result<common::DataSourceType>::success(common::DataSourceType::Synthetic);
    if (text == "REPLAYED") return common::Result<common::DataSourceType>::success(common::DataSourceType::Replayed);
    if (text == "DERIVED") return common::Result<common::DataSourceType>::success(common::DataSourceType::Derived);
    return common::Result<common::DataSourceType>::failure({"LEDGER_EVENT", "Unknown persisted data source"});
}

common::Result<common::FeedQualityState> parse_quality(const std::string_view text) {
    if (text == "OK") return common::Result<common::FeedQualityState>::success(common::FeedQualityState::Ok);
    if (text == "STALE") return common::Result<common::FeedQualityState>::success(common::FeedQualityState::Stale);
    if (text == "DEGRADED") return common::Result<common::FeedQualityState>::success(common::FeedQualityState::Degraded);
    if (text == "DISCONNECTED") return common::Result<common::FeedQualityState>::success(common::FeedQualityState::Disconnected);
    return common::Result<common::FeedQualityState>::failure({"LEDGER_EVENT", "Unknown persisted market-data quality"});
}

common::Result<std::int64_t> signed_quantity(const common::Quantity quantity) {
    if (quantity == 0 || quantity > static_cast<common::Quantity>(std::numeric_limits<std::int64_t>::max()))
        return common::Result<std::int64_t>::failure({"LEDGER_QUANTITY", "Fill quantity must fit a positive signed 64-bit position"});
    return common::Result<std::int64_t>::success(static_cast<std::int64_t>(quantity));
}

common::Result<std::int64_t> magnitude(const std::int64_t value) {
    if (value == std::numeric_limits<std::int64_t>::min())
        return common::Result<std::int64_t>::failure({"LEDGER_ARITHMETIC", "Position magnitude overflow"});
    return common::Result<std::int64_t>::success(value < 0 ? -value : value);
}

common::Result<std::int64_t> subtract(const std::int64_t left, const std::int64_t right) {
    if ((right < 0 && left > std::numeric_limits<std::int64_t>::max() + right) ||
        (right > 0 && left < std::numeric_limits<std::int64_t>::min() + right))
        return common::Result<std::int64_t>::failure({"LEDGER_ARITHMETIC", "Signed subtraction overflow"});
    return common::Result<std::int64_t>::success(left - right);
}

common::Result<std::int64_t> product(const std::int64_t price, const std::int64_t quantity) {
    return common::checked_multiply(price, quantity);
}

common::Result<void> refresh_mark(PositionState& position) {
    if (position.quantity == 0 || position.mark_source == "UNMARKED") {
        position.unrealized_pnl_micro = 0;
        return common::Result<void>::success();
    }
    if (position.mark_source == "AVERAGE_COST_FALLBACK_NO_QUOTE") {
        position.mark_micro = position.average_cost_micro;
        position.unrealized_pnl_micro = 0;
        return common::Result<void>::success();
    }
    if (!position.mark_micro) return common::Result<void>::failure({"MARK_INVALID", "Marked position has no mark value"});
    const auto absolute_quantity = magnitude(position.quantity);
    if (!absolute_quantity) return common::Result<void>::failure(absolute_quantity.error());
    const auto mark_value = product(*position.mark_micro, absolute_quantity.value());
    const auto average_basis = product(position.average_cost_micro, absolute_quantity.value());
    if (!mark_value || !average_basis) return common::Result<void>::failure({"LEDGER_ARITHMETIC", "Position mark or basis overflow"});
    const auto basis = common::checked_add(average_basis.value(), position.cost_residual);
    if (!basis) return common::Result<void>::failure({"LEDGER_ARITHMETIC", "Position basis residual overflow"});
    const auto pnl = position.quantity > 0 ? subtract(mark_value.value(), basis.value())
                                           : subtract(basis.value(), mark_value.value());
    if (!pnl) return common::Result<void>::failure(pnl.error());
    position.unrealized_pnl_micro = pnl.value();
    return common::Result<void>::success();
}

} // namespace

common::Result<void> PortfolioLedger::create_account(LedgerAccount account) {
    if (account.account_id == 0 || account.name.empty() || account.initial_cash_micro < 0)
        return common::Result<void>::failure({"ACCOUNT_INVALID", "Account ID, name and non-negative opening cash are required"});
    if (accounts_.contains(account.account_id))
        return common::Result<void>::failure({"ACCOUNT_EXISTS", "Account ID already exists"});
    account.cash_balance_micro = account.initial_cash_micro;
    if (sink_) {
        Json event = Json::object();
        event["kind"] = "ledger_account_seed";
        event["account_id"] = account.account_id;
        event["name"] = account.name;
        event["initial_cash_micro"] = account.initial_cash_micro;
        event["allow_short"] = account.allow_short;
        const auto saved = sink_(event.dump());
        if (!saved) return saved;
    }
    accounts_.emplace(account.account_id, std::move(account));
    return common::Result<void>::success();
}

PortfolioLedger::EventSink PortfolioLedger::event_log_sink(persistence::EventLog& log) {
    return [&log](const std::string& payload) {
        const auto appended = log.append_next(payload);
        if (!appended) return common::Result<void>::failure(appended.error());
        return common::Result<void>::success();
    };
}

common::Result<void> PortfolioLedger::restore(const std::vector<persistence::LogFrame>& frames) {
    PortfolioLedger restored;
    for (const auto& frame : frames) {
        Json event;
        try {
            event = Json::parse(frame.payload);
        } catch (const std::exception&) {
            continue;
        }
        if (!event.is_object()) continue;
        const auto kind = event.value("kind", std::string{});
        if (kind == "ledger_account_seed") {
            LedgerAccount account;
            try {
                account.account_id = event.at("account_id").get<common::AccountId>();
                account.name = event.at("name").get<std::string>();
                account.initial_cash_micro = event.at("initial_cash_micro").get<std::int64_t>();
                account.allow_short = event.at("allow_short").get<bool>();
            } catch (const std::exception&) {
                return common::Result<void>::failure({"LEDGER_EVENT", "Persisted account seed is malformed"});
            }
            const auto seeded = restored.create_account(std::move(account));
            if (!seeded) return seeded;
        } else if (kind == "ledger_fill") {
            ExecutionFill fill;
            try {
                const auto side = parse_side(event.at("side").get<std::string>());
                const auto model = parse_model(event.at("model").get<std::string>());
                const auto source = parse_source(event.at("source").get<std::string>());
                const auto quality = parse_quality(event.at("quality").get<std::string>());
                if (!side || !model || !source || !quality)
                    return common::Result<void>::failure({"LEDGER_EVENT", "Persisted fill has an unknown enum value"});
                fill.execution_id = event.at("execution_id").get<std::uint64_t>();
                fill.event_sequence = event.at("event_sequence").get<std::uint64_t>();
                fill.order_id = event.at("order_id").get<std::uint64_t>();
                fill.account_id = event.at("account_id").get<common::AccountId>();
                fill.instrument_id = event.at("instrument_id").get<common::InstrumentId>();
                fill.strategy_id = event.at("strategy_id").get<common::StrategyId>();
                fill.side = side.value();
                fill.price_micro = event.at("price_micro").get<common::MicroUnits>();
                fill.quantity = event.at("quantity").get<common::Quantity>();
                fill.fee_micro = event.at("fee_micro").get<std::int64_t>();
                fill.model = model.value();
                fill.model_version = event.at("model_version").get<std::uint32_t>();
                fill.source = source.value();
                fill.quality = quality.value();
                fill.fee_schedule_id = event.at("fee_schedule_id").get<std::string>();
                fill.fee_schedule_version = event.at("fee_schedule_version").get<std::uint32_t>();
                fill.fee_schedule_source = event.at("fee_schedule_source").get<std::string>();
                fill.time_ns = event.at("time_ns").get<std::int64_t>();
            } catch (const std::exception&) {
                return common::Result<void>::failure({"LEDGER_EVENT", "Persisted fill event is malformed"});
            }
            const auto applied = restored.apply_fill(fill);
            if (!applied) return applied;
        }
    }
    accounts_ = std::move(restored.accounts_);
    positions_ = std::move(restored.positions_);
    execution_ids_ = std::move(restored.execution_ids_);
    event_sequences_ = std::move(restored.event_sequences_);
    journal_ = std::move(restored.journal_);
    fills_ = std::move(restored.fills_);
    last_event_sequence_ = restored.last_event_sequence_;
    return common::Result<void>::success();
}

common::Result<void> PortfolioLedger::apply_fill(const ExecutionFill& fill) {
    if (fill.execution_id == 0 || fill.event_sequence == 0 || fill.order_id == 0 ||
        fill.account_id == 0 || fill.instrument_id == 0 || fill.time_ns <= 0 ||
        fill.price_micro <= 0 || fill.fee_micro < 0 || fill.fee_schedule_id.empty() ||
        fill.fee_schedule_version == 0 || fill.fee_schedule_source.empty())
        return common::Result<void>::failure({"FILL_INVALID", "Execution report has invalid identity, time, price, fee or provenance"});
    if (execution_ids_.contains(fill.execution_id) || event_sequences_.contains(fill.event_sequence))
        return common::Result<void>::failure({"DUPLICATE_EXECUTION", "Execution ID and event sequence must be unique"});
    if (fill.event_sequence <= last_event_sequence_)
        return common::Result<void>::failure({"EVENT_SEQUENCE", "Ledger event sequences must increase strictly"});
    const auto account_it = accounts_.find(fill.account_id);
    if (account_it == accounts_.end()) return common::Result<void>::failure({"ACCOUNT_NOT_FOUND", "Fill account has not been seeded"});
    const auto quantity = signed_quantity(fill.quantity);
    if (!quantity) return common::Result<void>::failure(quantity.error());
    const auto notional = product(fill.price_micro, quantity.value());
    if (!notional || notional.value() <= 0)
        return common::Result<void>::failure({"LEDGER_ARITHMETIC", "Fill notional is invalid or overflowed"});

    const auto current_position = positions_.find({fill.account_id, fill.instrument_id});
    PositionState next_position = current_position == positions_.end() ? PositionState{} : current_position->second;
    const auto signed_fill = fill.side == Side::Buy ? quantity.value() : -quantity.value();
    const auto new_quantity = common::checked_add(next_position.quantity, signed_fill);
    if (!new_quantity) return common::Result<void>::failure({"LEDGER_ARITHMETIC", "Position quantity overflow"});
    if (!account_it->second.allow_short && new_quantity.value() < 0)
        return common::Result<void>::failure({"INSUFFICIENT_POSITION", "Account is not configured to open or increase a short position"});

    std::int64_t cash_debit = 0;
    std::int64_t cash_credit = 0;
    std::int64_t next_cash = account_it->second.cash_balance_micro;
    if (fill.side == Side::Buy) {
        const auto outflow = common::checked_add(notional.value(), fill.fee_micro);
        if (!outflow) return common::Result<void>::failure({"LEDGER_ARITHMETIC", "Buy cash debit overflow"});
        if (outflow.value() > next_cash)
            return common::Result<void>::failure({"INSUFFICIENT_CASH", "Account cash is insufficient for notional and fees"});
        cash_debit = outflow.value();
        const auto balance = subtract(next_cash, cash_debit);
        if (!balance) return common::Result<void>::failure(balance.error());
        next_cash = balance.value();
    } else {
        if (fill.fee_micro > notional.value())
            return common::Result<void>::failure({"LEDGER_ARITHMETIC", "Sell fee exceeds sale proceeds"});
        cash_credit = notional.value() - fill.fee_micro;
        const auto balance = common::checked_add(next_cash, cash_credit);
        if (!balance) return common::Result<void>::failure({"LEDGER_ARITHMETIC", "Sell cash credit overflow"});
        next_cash = balance.value();
    }

    const auto old_abs = magnitude(next_position.quantity);
    if (!old_abs) return common::Result<void>::failure(old_abs.error());
    const bool same_direction = next_position.quantity == 0 ||
        ((next_position.quantity > 0) == (signed_fill > 0));
    const auto previous_quantity = next_position.quantity;
    if (same_direction) {
        const auto prior_basis = product(next_position.average_cost_micro, old_abs.value());
        if (!prior_basis) return common::Result<void>::failure({"LEDGER_ARITHMETIC", "Prior cost basis overflow"});
        const auto with_residual = common::checked_add(prior_basis.value(), next_position.cost_residual);
        if (!with_residual) return common::Result<void>::failure({"LEDGER_ARITHMETIC", "Prior cost residual overflow"});
        const auto total_basis = common::checked_add(with_residual.value(), notional.value());
        if (!total_basis) return common::Result<void>::failure({"LEDGER_ARITHMETIC", "Weighted cost basis overflow"});
        const auto new_abs = magnitude(new_quantity.value());
        if (!new_abs || new_abs.value() == 0) return common::Result<void>::failure({"LEDGER_ARITHMETIC", "Invalid updated position size"});
        next_position.quantity = new_quantity.value();
        next_position.average_cost_micro = total_basis.value() / new_abs.value();
        next_position.cost_residual = total_basis.value() % new_abs.value();
    } else {
        const auto closed_quantity = std::min(old_abs.value(), quantity.value());
        const bool closes_position = closed_quantity == old_abs.value();
        auto closed_basis = product(next_position.average_cost_micro, closed_quantity);
        if (!closed_basis) return common::Result<void>::failure({"LEDGER_ARITHMETIC", "Closed cost basis overflow"});
        if (closes_position) {
            closed_basis = common::checked_add(closed_basis.value(), next_position.cost_residual);
            if (!closed_basis) return common::Result<void>::failure({"LEDGER_ARITHMETIC", "Closed cost residual overflow"});
        }
        const auto close_notional = product(fill.price_micro, closed_quantity);
        if (!close_notional) return common::Result<void>::failure({"LEDGER_ARITHMETIC", "Closed fill notional overflow"});
        const auto realized_delta = next_position.quantity > 0
            ? subtract(close_notional.value(), closed_basis.value())
            : subtract(closed_basis.value(), close_notional.value());
        if (!realized_delta) return common::Result<void>::failure(realized_delta.error());
        const auto realized = common::checked_add(next_position.realized_pnl_micro, realized_delta.value());
        if (!realized) return common::Result<void>::failure({"LEDGER_ARITHMETIC", "Realized P&L overflow"});
        next_position.realized_pnl_micro = realized.value();
        next_position.quantity = new_quantity.value();
        if (new_quantity.value() == 0) {
            next_position.average_cost_micro = 0;
            next_position.cost_residual = 0;
            next_position.mark_micro.reset();
            next_position.mark_source = "UNMARKED";
            next_position.unrealized_pnl_micro = 0;
        } else if ((new_quantity.value() > 0) != (previous_quantity > 0)) {
            next_position.average_cost_micro = fill.price_micro;
            next_position.cost_residual = 0;
            next_position.mark_micro.reset();
            next_position.mark_source = "UNMARKED";
            next_position.unrealized_pnl_micro = 0;
        }
    }
    const auto accumulated_fees = common::checked_add(next_position.fees_micro, fill.fee_micro);
    if (!accumulated_fees) return common::Result<void>::failure({"LEDGER_ARITHMETIC", "Position fee accumulator overflow"});
    next_position.fees_micro = accumulated_fees.value();
    next_position.updated_sequence = fill.event_sequence;
    const auto updated_mark = refresh_mark(next_position);
    if (!updated_mark) return updated_mark;

    CashJournalEntry entry;
    entry.execution_id = fill.execution_id;
    entry.event_sequence = fill.event_sequence;
    entry.account_id = fill.account_id;
    entry.debit_micro = cash_debit;
    entry.credit_micro = cash_credit;
    entry.contra_credit_micro = cash_debit;
    entry.contra_debit_micro = cash_credit;
    entry.fee_micro = fill.fee_micro;
    entry.balance_micro = next_cash;
    entry.memo = std::string(to_string(fill.model)) + ":" + std::string(common::to_string(fill.source)) +
                 ":" + fill.fee_schedule_id + "@v" + std::to_string(fill.fee_schedule_version) +
                 ":" + fill.fee_schedule_source;

    if (sink_) {
        Json event = Json::object();
        event["kind"] = "ledger_fill";
        event["execution_id"] = fill.execution_id;
        event["event_sequence"] = fill.event_sequence;
        event["order_id"] = fill.order_id;
        event["account_id"] = fill.account_id;
        event["instrument_id"] = fill.instrument_id;
        event["strategy_id"] = fill.strategy_id;
        event["side"] = side_name(fill.side);
        event["price_micro"] = fill.price_micro;
        event["quantity"] = fill.quantity;
        event["fee_micro"] = fill.fee_micro;
        event["model"] = to_string(fill.model);
        event["model_version"] = fill.model_version;
        event["source"] = common::to_string(fill.source);
        event["quality"] = common::to_string(fill.quality);
        event["fee_schedule_id"] = fill.fee_schedule_id;
        event["fee_schedule_version"] = fill.fee_schedule_version;
        event["fee_schedule_source"] = fill.fee_schedule_source;
        event["time_ns"] = fill.time_ns;
        const auto saved = sink_(event.dump());
        if (!saved) return saved;
    }

    account_it->second.cash_balance_micro = next_cash;
    positions_[{fill.account_id, fill.instrument_id}] = next_position;
    execution_ids_.insert(fill.execution_id);
    event_sequences_.insert(fill.event_sequence);
    last_event_sequence_ = fill.event_sequence;
    journal_.push_back(std::move(entry));
    fills_.push_back(fill);
    return common::Result<void>::success();
}

common::Result<void> PortfolioLedger::mark(const common::AccountId account_id,
                                           const common::InstrumentId instrument_id,
                                           const std::optional<common::MicroUnits> bid_micro,
                                           const std::optional<common::MicroUnits> ask_micro) {
    if (!accounts_.contains(account_id)) return common::Result<void>::failure({"ACCOUNT_NOT_FOUND", "Cannot mark an unknown account"});
    const auto found = positions_.find({account_id, instrument_id});
    if (found == positions_.end()) return common::Result<void>::failure({"POSITION_NOT_FOUND", "Cannot mark an unknown position"});
    auto& position = found->second;
    if (position.quantity == 0) return common::Result<void>::success();
    const bool is_long = position.quantity > 0;
    const auto observed = is_long ? bid_micro : ask_micro;
    if (!observed) {
        position.mark_micro = position.average_cost_micro;
        position.mark_source = "AVERAGE_COST_FALLBACK_NO_QUOTE";
        position.unrealized_pnl_micro = 0;
        return common::Result<void>::success();
    }
    if (*observed <= 0) return common::Result<void>::failure({"MARK_INVALID", "Conservative mark must be positive"});
    const auto absolute_quantity = magnitude(position.quantity);
    if (!absolute_quantity) return common::Result<void>::failure(absolute_quantity.error());
    const auto mark_value = product(*observed, absolute_quantity.value());
    const auto average_basis = product(position.average_cost_micro, absolute_quantity.value());
    if (!mark_value || !average_basis) return common::Result<void>::failure({"LEDGER_ARITHMETIC", "Mark or position basis overflow"});
    const auto total_basis = common::checked_add(average_basis.value(), position.cost_residual);
    if (!total_basis) return common::Result<void>::failure({"LEDGER_ARITHMETIC", "Position basis residual overflow"});
    const auto unrealized = is_long ? subtract(mark_value.value(), total_basis.value())
                                    : subtract(total_basis.value(), mark_value.value());
    if (!unrealized) return common::Result<void>::failure(unrealized.error());
    position.mark_micro = *observed;
    position.mark_source = is_long ? "CONSERVATIVE_BID" : "CONSERVATIVE_ASK";
    position.unrealized_pnl_micro = unrealized.value();
    return common::Result<void>::success();
}

common::Result<LedgerAccount> PortfolioLedger::account(const common::AccountId account_id) const {
    const auto found = accounts_.find(account_id);
    if (found == accounts_.end()) return common::Result<LedgerAccount>::failure({"ACCOUNT_NOT_FOUND", "Account ID is not registered"});
    return common::Result<LedgerAccount>::success(found->second);
}

common::Result<std::optional<PositionState>> PortfolioLedger::position(
    const common::AccountId account_id, const common::InstrumentId instrument_id) const {
    if (!accounts_.contains(account_id)) return common::Result<std::optional<PositionState>>::failure({"ACCOUNT_NOT_FOUND", "Account ID is not registered"});
    const auto found = positions_.find({account_id, instrument_id});
    if (found == positions_.end()) return common::Result<std::optional<PositionState>>::success(std::nullopt);
    return common::Result<std::optional<PositionState>>::success(found->second);
}

common::Result<PortfolioSummary> PortfolioLedger::summary(const common::AccountId account_id) const {
    const auto found = accounts_.find(account_id);
    if (found == accounts_.end()) return common::Result<PortfolioSummary>::failure({"ACCOUNT_NOT_FOUND", "Account ID is not registered"});
    PortfolioSummary summary;
    summary.account_id = account_id;
    summary.cash_balance_micro = found->second.cash_balance_micro;
    for (const auto& [key, position] : positions_) {
        if (key.first != account_id) continue;
        auto fees = common::checked_add(summary.total_fees_micro, position.fees_micro);
        auto realized = common::checked_add(summary.realized_pnl_micro, position.realized_pnl_micro);
        auto unrealized = common::checked_add(summary.unrealized_pnl_micro, position.unrealized_pnl_micro);
        if (!fees || !realized || !unrealized)
            return common::Result<PortfolioSummary>::failure({"LEDGER_ARITHMETIC", "Portfolio summary overflow"});
        summary.total_fees_micro = fees.value();
        summary.realized_pnl_micro = realized.value();
        summary.unrealized_pnl_micro = unrealized.value();
        summary.positions.emplace_back(key.second, position);
    }
    const auto net_realized = subtract(summary.realized_pnl_micro, summary.total_fees_micro);
    if (!net_realized) return common::Result<PortfolioSummary>::failure(net_realized.error());
    summary.realized_pnl_micro = net_realized.value();
    return common::Result<PortfolioSummary>::success(std::move(summary));
}

common::Result<void> PortfolioLedger::verify_reconciliation() const {
    std::map<common::AccountId, std::int64_t> cash_balances;
    std::map<PositionKey, std::int64_t> expected_positions;
    std::map<PositionKey, std::int64_t> expected_fees;
    std::set<std::uint64_t> execution_ids;
    std::set<std::uint64_t> sequences;
    std::uint64_t previous_sequence = 0;
    for (const auto& [account_id, account] : accounts_) cash_balances[account_id] = account.initial_cash_micro;
    if (journal_.size() != fills_.size()) return common::Result<void>::failure({"RECONCILIATION_FAILED", "Cash journal and execution counts differ"});
    for (std::size_t i = 0; i < fills_.size(); ++i) {
        const auto& fill = fills_[i];
        const auto& entry = journal_[i];
        if (fill.event_sequence <= previous_sequence || !execution_ids.insert(fill.execution_id).second || !sequences.insert(fill.event_sequence).second ||
            entry.execution_id != fill.execution_id || entry.event_sequence != fill.event_sequence ||
            entry.account_id != fill.account_id || entry.fee_micro != fill.fee_micro)
            return common::Result<void>::failure({"RECONCILIATION_FAILED", "Execution and journal identities do not reconcile"});
        previous_sequence = fill.event_sequence;
        const auto debits = common::checked_add(entry.debit_micro, entry.contra_debit_micro);
        const auto credits = common::checked_add(entry.credit_micro, entry.contra_credit_micro);
        if (!debits || !credits || debits.value() != credits.value())
            return common::Result<void>::failure({"RECONCILIATION_FAILED", "Double-entry postings do not balance"});
        const auto fill_quantity = signed_quantity(fill.quantity);
        if (!fill_quantity) return common::Result<void>::failure(fill_quantity.error());
        const auto fill_notional = product(fill.price_micro, fill_quantity.value());
        if (!fill_notional) return common::Result<void>::failure(fill_notional.error());
        const auto expected_cash = fill.side == Side::Buy
            ? common::checked_add(fill_notional.value(), fill.fee_micro)
            : common::Result<std::int64_t>::success(fill_notional.value() - fill.fee_micro);
        if (!expected_cash || (fill.side == Side::Buy
                ? (entry.debit_micro != expected_cash.value() || entry.credit_micro != 0 || entry.contra_credit_micro != expected_cash.value())
                : (entry.credit_micro != expected_cash.value() || entry.debit_micro != 0 || entry.contra_debit_micro != expected_cash.value())))
            return common::Result<void>::failure({"RECONCILIATION_FAILED", "Cash and contra postings differ from fill notional and fee"});
        auto balance = subtract(cash_balances[fill.account_id], entry.debit_micro);
        if (!balance) return common::Result<void>::failure(balance.error());
        balance = common::checked_add(balance.value(), entry.credit_micro);
        if (!balance || balance.value() != entry.balance_micro)
            return common::Result<void>::failure({"RECONCILIATION_FAILED", "Cash ledger running balance is inconsistent"});
        cash_balances[fill.account_id] = balance.value();
        const auto signed_qty = signed_quantity(fill.quantity);
        if (!signed_qty) return common::Result<void>::failure(signed_qty.error());
        const auto delta = fill.side == Side::Buy ? signed_qty.value() : -signed_qty.value();
        const auto position = common::checked_add(expected_positions[{fill.account_id, fill.instrument_id}], delta);
        const auto fees = common::checked_add(expected_fees[{fill.account_id, fill.instrument_id}], fill.fee_micro);
        if (!position || !fees) return common::Result<void>::failure({"RECONCILIATION_FAILED", "Fill aggregation overflow"});
        expected_positions[{fill.account_id, fill.instrument_id}] = position.value();
        expected_fees[{fill.account_id, fill.instrument_id}] = fees.value();
    }
    for (const auto& [account_id, account] : accounts_) {
        if (cash_balances[account_id] != account.cash_balance_micro)
            return common::Result<void>::failure({"RECONCILIATION_FAILED", "Account cash does not match journal cash flows"});
    }
    for (const auto& [key, expected] : expected_positions) {
        const auto current = positions_.find(key);
        if (current == positions_.end() || current->second.quantity != expected ||
            current->second.fees_micro != expected_fees[key])
            return common::Result<void>::failure({"RECONCILIATION_FAILED", "Position quantity or fee total does not match execution fills"});
    }
    for (const auto& [key, position] : positions_) {
        if (!expected_positions.contains(key) && (position.quantity != 0 || position.fees_micro != 0))
            return common::Result<void>::failure({"RECONCILIATION_FAILED", "Position has no supporting execution fills"});
    }

    PortfolioLedger replay;
    for (const auto& [unused, account] : accounts_) {
        (void)unused;
        const auto seeded = replay.create_account({account.account_id, account.name,
                                                    account.initial_cash_micro, 0,
                                                    account.allow_short});
        if (!seeded) return common::Result<void>::failure({"RECONCILIATION_FAILED", "Could not replay account seed"});
    }
    for (const auto& fill : fills_) {
        const auto applied = replay.apply_fill(fill);
        if (!applied) return common::Result<void>::failure({"RECONCILIATION_FAILED", "Execution replay failed: " + applied.error().message});
    }
    for (const auto& [key, position] : positions_) {
        const auto expected = replay.positions_.find(key);
        if (expected == replay.positions_.end() || position.quantity != expected->second.quantity ||
            position.average_cost_micro != expected->second.average_cost_micro ||
            position.cost_residual != expected->second.cost_residual ||
            position.realized_pnl_micro != expected->second.realized_pnl_micro ||
            position.fees_micro != expected->second.fees_micro ||
            position.updated_sequence != expected->second.updated_sequence)
            return common::Result<void>::failure({"RECONCILIATION_FAILED", "Position cost basis or realized P&L differs from execution replay"});
    }
    return common::Result<void>::success();
}

} // namespace basalt::trading
