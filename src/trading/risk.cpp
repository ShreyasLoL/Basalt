#include "basalt/trading/risk.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <sstream>

namespace basalt::trading {
namespace {

using Json = nlohmann::json;

std::string_view scope_name(const KillScope scope) {
    switch (scope) {
    case KillScope::Global: return "GLOBAL";
    case KillScope::Account: return "ACCOUNT";
    case KillScope::Strategy: return "STRATEGY";
    }
    return "UNKNOWN";
}

std::string_view side_name(const Side side) { return side == Side::Buy ? "BUY" : "SELL"; }
std::string_view order_type_name(const OrderType type) { return type == OrderType::Market ? "MARKET" : "LIMIT"; }

common::Result<Side> parse_side(const std::string_view text) {
    if (text == "BUY") return common::Result<Side>::success(Side::Buy);
    if (text == "SELL") return common::Result<Side>::success(Side::Sell);
    return common::Result<Side>::failure({"RISK_EVENT", "Unknown persisted order side"});
}

common::Result<OrderType> parse_order_type(const std::string_view text) {
    if (text == "MARKET") return common::Result<OrderType>::success(OrderType::Market);
    if (text == "LIMIT") return common::Result<OrderType>::success(OrderType::Limit);
    return common::Result<OrderType>::failure({"RISK_EVENT", "Unknown persisted order type"});
}

common::Result<KillScope> parse_scope(const std::string_view text) {
    if (text == "GLOBAL") return common::Result<KillScope>::success(KillScope::Global);
    if (text == "ACCOUNT") return common::Result<KillScope>::success(KillScope::Account);
    if (text == "STRATEGY") return common::Result<KillScope>::success(KillScope::Strategy);
    return common::Result<KillScope>::failure({"KILL_SWITCH_FORMAT", "Unknown persisted kill-switch scope"});
}

common::Result<std::int64_t> subtract(const std::int64_t left, const std::int64_t right) {
    if ((right < 0 && left > std::numeric_limits<std::int64_t>::max() + right) ||
        (right > 0 && left < std::numeric_limits<std::int64_t>::min() + right)) {
        return common::Result<std::int64_t>::failure({"RISK_CALCULATION_ERROR", "Signed subtraction overflow"});
    }
    return common::Result<std::int64_t>::success(left - right);
}

common::Result<std::int64_t> signed_quantity(const common::Quantity quantity) {
    if (quantity > static_cast<common::Quantity>(std::numeric_limits<std::int64_t>::max()))
        return common::Result<std::int64_t>::failure({"RISK_CALCULATION_ERROR", "Order quantity exceeds signed position range"});
    return common::Result<std::int64_t>::success(static_cast<std::int64_t>(quantity));
}

common::Result<std::int64_t> magnitude(const common::PositionQuantity value) {
    if (value == std::numeric_limits<std::int64_t>::min())
        return common::Result<std::int64_t>::failure({"RISK_CALCULATION_ERROR", "Position magnitude exceeds signed range"});
    return common::Result<std::int64_t>::success(value < 0 ? -value : value);
}

common::Result<std::int64_t> notional(const common::MicroUnits price,
                                      const common::Quantity quantity) {
    if (price <= 0) return common::Result<std::int64_t>::failure({"DATA_UNAVAILABLE", "A positive conservative reference price is required"});
    const auto signed_qty = signed_quantity(quantity);
    if (!signed_qty) return signed_qty;
    return common::checked_multiply(price, signed_qty.value());
}

common::Result<std::int64_t> nonnegative_loss(const std::int64_t pnl) {
    if (pnl >= 0) return common::Result<std::int64_t>::success(0);
    if (pnl == std::numeric_limits<std::int64_t>::min())
        return common::Result<std::int64_t>::failure({"RISK_CALCULATION_ERROR", "Loss exceeds signed fixed-point range"});
    return common::Result<std::int64_t>::success(-pnl);
}

bool exceeds_fraction(const std::int64_t part, const std::int64_t total,
                      const std::uint32_t basis_points) {
    if (part <= 0 || total <= 0) return false;
    const auto whole = total / 10'000;
    const auto remainder = total % 10'000;
    const auto threshold = whole * static_cast<std::int64_t>(basis_points) +
                           (remainder * static_cast<std::int64_t>(basis_points)) / 10'000;
    return part > threshold;
}

RiskDecision rejected(std::string code) {
    RiskDecision decision;
    decision.code = std::move(code);
    return decision;
}

} // namespace

KillSwitch::KillSwitch(EventSink sink, CancelSink cancel_sink)
    : sink_(std::move(sink)), cancel_sink_(std::move(cancel_sink)) {}

KillSwitch::EventSink KillSwitch::event_log_sink(persistence::EventLog& log) {
    return [&log](const KillSwitchEvent& event) {
        Json json = Json::object();
        json["kind"] = "kill_switch";
        json["sequence"] = event.sequence;
        json["scope"] = scope_name(event.scope);
        json["target_id"] = event.target_id;
        json["active"] = event.active;
        json["actor"] = event.actor;
        json["reason"] = event.reason;
        json["time_ns"] = event.time_ns;
        json["working_orders_cancelled"] = event.working_orders_cancelled;
        const auto appended = log.append_next(json.dump());
        if (!appended) return common::Result<void>::failure(appended.error());
        return common::Result<void>::success();
    };
}

common::Result<void> KillSwitch::restore(const std::vector<persistence::LogFrame>& frames) {
    std::lock_guard lock(mutex_);
    for (const auto& frame : frames) {
        Json json;
        try {
            json = Json::parse(frame.payload);
        } catch (const std::exception&) {
            continue;
        }
        if (!json.is_object() || json.value("kind", std::string{}) != "kill_switch") continue;
        try {
            auto scope = parse_scope(json.at("scope").get<std::string>());
            if (!scope) return common::Result<void>::failure(scope.error());
            KillSwitchEvent event;
            event.sequence = json.at("sequence").get<std::uint64_t>();
            event.scope = scope.value();
            event.target_id = json.at("target_id").get<std::uint64_t>();
            event.active = json.at("active").get<bool>();
            event.actor = json.value("actor", std::string{});
            event.reason = json.value("reason", std::string{});
            event.time_ns = json.at("time_ns").get<std::int64_t>();
            event.working_orders_cancelled = json.value("working_orders_cancelled", false);
            event.persisted = true;
            states_[{event.scope, event.target_id}] = event.active;
            state_sequences_[{event.scope, event.target_id}] = event.sequence;
            next_sequence_ = std::max(next_sequence_, event.sequence + 1U);
            events_.push_back(std::move(event));
        } catch (const std::exception&) {
            return common::Result<void>::failure({"KILL_SWITCH_FORMAT", "Persisted kill-switch event is malformed"});
        }
    }
    return common::Result<void>::success();
}

KillSwitchEvent KillSwitch::activate(const KillScope scope, const std::uint64_t target_id,
                                     std::string actor, std::string reason,
                                     const std::int64_t time_ns) {
    const auto canonical_target = scope == KillScope::Global ? 0U : target_id;
    const Key key{scope, canonical_target};
    KillSwitchEvent event;
    EventSink sink;
    CancelSink cancel_sink;
    {
        std::lock_guard lock(mutex_);
        const auto sequence = next_sequence_++;
        event = KillSwitchEvent{sequence, scope, canonical_target, true, std::move(actor),
                                std::move(reason), time_ns, false, false};
        states_[key] = true; // Latch before attempting fallible actions.
        state_sequences_[key] = sequence;
        sink = sink_;
        cancel_sink = cancel_sink_;
    }
    if (cancel_sink) {
        try { event.working_orders_cancelled = static_cast<bool>(cancel_sink(scope, canonical_target)); }
        catch (const std::exception&) { event.working_orders_cancelled = false; }
    }
    if (sink && event.time_ns > 0 && !event.actor.empty() && !event.reason.empty()) {
        try {
            const auto saved = sink(event);
            event.persisted = static_cast<bool>(saved);
        } catch (const std::exception&) {
            event.persisted = false;
        }
    }
    {
        std::lock_guard lock(mutex_);
        if (state_sequences_[key] <= event.sequence) {
            states_[key] = true;
            state_sequences_[key] = event.sequence;
        }
        events_.push_back(event);
    }
    return event;
}

common::Result<KillSwitchEvent> KillSwitch::reset(const KillScope scope,
                                                  const std::uint64_t target_id,
                                                  std::string actor,
                                                  std::string reason,
                                                  const std::int64_t time_ns,
                                                  const bool authorized,
                                                  const bool health_checks_passed) {
    if (!authorized || !health_checks_passed || time_ns <= 0 || actor.empty() || reason.empty())
        return common::Result<KillSwitchEvent>::failure({"KILL_SWITCH_RESET_DENIED", "Reset requires authorization, a reason, valid time and passing health checks"});
    const auto canonical_target = scope == KillScope::Global ? 0U : target_id;
    const Key key{scope, canonical_target};
    KillSwitchEvent event;
    EventSink sink;
    {
        std::lock_guard lock(mutex_);
        if (!sink_)
            return common::Result<KillSwitchEvent>::failure({"KILL_SWITCH_JOURNAL", "Cannot clear a kill switch without a durable event journal"});
        event = KillSwitchEvent{next_sequence_++, scope, canonical_target, false, std::move(actor),
                                std::move(reason), time_ns, false, false};
        sink = sink_;
    }
    common::Result<void> saved = common::Result<void>::failure({"KILL_SWITCH_JOURNAL", "Kill-switch journal failed"});
    try {
        saved = sink(event);
    } catch (const std::exception&) {
        std::lock_guard lock(mutex_);
        if (next_sequence_ == event.sequence + 1U) --next_sequence_;
        return common::Result<KillSwitchEvent>::failure({"KILL_SWITCH_JOURNAL", "Kill-switch journal threw an exception"});
    }
    if (!saved) {
        std::lock_guard lock(mutex_);
        if (next_sequence_ == event.sequence + 1U) --next_sequence_;
        return common::Result<KillSwitchEvent>::failure({"KILL_SWITCH_JOURNAL", saved.error().message});
    }
    event.persisted = true;
    {
        std::lock_guard lock(mutex_);
        if (state_sequences_[key] <= event.sequence) {
            states_[key] = false;
            state_sequences_[key] = event.sequence;
        }
        events_.push_back(event);
    }
    return common::Result<KillSwitchEvent>::success(std::move(event));
}

bool KillSwitch::active(const common::AccountId account_id,
                        const common::StrategyId strategy_id) const {
    std::lock_guard lock(mutex_);
    const auto is_active = [&](const Key key) {
        const auto found = states_.find(key);
        return found != states_.end() && found->second;
    };
    return is_active({KillScope::Global, 0}) || is_active({KillScope::Account, account_id}) ||
           (strategy_id != 0 && is_active({KillScope::Strategy, strategy_id}));
}

std::vector<KillSwitchEvent> KillSwitch::events() const {
    std::lock_guard lock(mutex_);
    return events_;
}

RiskEngine::EventSink RiskEngine::event_log_sink(persistence::EventLog& log) {
    return [&log](const std::string& payload) {
        const auto appended = log.append_next(payload);
        if (!appended) return common::Result<void>::failure(appended.error());
        return common::Result<void>::success();
    };
}

common::Result<void> RiskEngine::restore(const std::vector<persistence::LogFrame>& frames) {
    std::lock_guard lock(mutex_);
    for (const auto& frame : frames) {
        Json event;
        try {
            event = Json::parse(frame.payload);
        } catch (const std::exception&) {
            continue;
        }
        if (!event.is_object() || event.value("kind", std::string{}) != "risk_decision" ||
            !event.value("accepted", false)) continue;
        try {
            const auto& order_json = event.at("intent");
            OrderIntent intent;
            intent.client_order_id = order_json.at("client_order_id").get<std::string>();
            intent.order_id = order_json.at("order_id").get<std::uint64_t>();
            intent.account_id = order_json.at("account_id").get<common::AccountId>();
            intent.instrument_id = order_json.at("instrument_id").get<common::InstrumentId>();
            intent.strategy_id = order_json.at("strategy_id").get<common::StrategyId>();
            const auto side = parse_side(order_json.at("side").get<std::string>());
            const auto type = parse_order_type(order_json.at("type").get<std::string>());
            if (!side || !type) return common::Result<void>::failure({"RISK_EVENT", "Persisted order has an unknown enum value"});
            intent.side = side.value();
            intent.type = type.value();
            intent.quantity = order_json.at("quantity").get<common::Quantity>();
            intent.estimated_fee_micro = order_json.at("estimated_fee_micro").get<std::int64_t>();
            if (!order_json.at("limit_price_micro").is_null())
                intent.limit_price_micro = order_json.at("limit_price_micro").get<common::MicroUnits>();
            intent.tick_size_micro = order_json.at("tick_size_micro").get<common::MicroUnits>();
            intent.lot_size = order_json.at("lot_size").get<common::Quantity>();
            intent.created_at_ns = order_json.at("created_at_ns").get<std::int64_t>();
            if (intent.client_order_id.empty()) return common::Result<void>::failure({"RISK_EVENT", "Persisted accepted order has an empty id"});
            const auto id = idempotency_.find(intent.client_order_id);
            const auto payload = signature(intent);
            if (id != idempotency_.end() && id->second != payload)
                return common::Result<void>::failure({"RISK_EVENT_DUPLICATE", "Conflicting accepted payloads use the same client order ID"});
            idempotency_[intent.client_order_id] = payload;
            if (!event.value("duplicate_retry", false))
                order_times_[intent.account_id].push_back(event.at("evaluated_at_ns").get<std::int64_t>());
        } catch (const std::exception&) {
            return common::Result<void>::failure({"RISK_EVENT", "Persisted accepted risk decision is malformed"});
        }
    }
    return common::Result<void>::success();
}

RiskDecision RiskEngine::evaluate(const OrderIntent& intent, const RiskSnapshot& snapshot,
                                  const RiskLimits& limits) {
    std::lock_guard serial(evaluation_mutex_);
    auto decision = evaluate_unlogged(intent, snapshot, limits);
    if (!sink_) return decision;
    Json event = Json::object();
    event["kind"] = "risk_decision";
    event["accepted"] = decision.accepted;
    event["duplicate_retry"] = decision.duplicate_retry;
    event["code"] = decision.code;
    event["reference_price_micro"] = decision.reference_price_micro;
    event["order_notional_micro"] = decision.order_notional_micro;
    event["evaluated_at_ns"] = snapshot.now_ns;
    Json order = Json::object();
    order["client_order_id"] = intent.client_order_id;
    order["order_id"] = intent.order_id;
    order["account_id"] = intent.account_id;
    order["instrument_id"] = intent.instrument_id;
    order["strategy_id"] = intent.strategy_id;
    order["side"] = side_name(intent.side);
    order["type"] = order_type_name(intent.type);
    order["quantity"] = intent.quantity;
    order["estimated_fee_micro"] = intent.estimated_fee_micro;
    order["limit_price_micro"] = intent.limit_price_micro ? Json(*intent.limit_price_micro) : Json(nullptr);
    order["tick_size_micro"] = intent.tick_size_micro;
    order["lot_size"] = intent.lot_size;
    order["created_at_ns"] = intent.created_at_ns;
    event["intent"] = std::move(order);
    Json checks = Json::array();
    for (const auto& check : decision.checks) {
        checks.push_back(Json{{"rule", check.rule}, {"rejection_code", check.rejection_code},
                              {"passed", check.passed}, {"observed", check.observed},
                              {"limit", check.limit}});
    }
    event["checks"] = std::move(checks);
    common::Result<void> saved = common::Result<void>::failure({"RISK_AUDIT", "Risk audit sink failed"});
    try {
        saved = sink_(event.dump());
    } catch (const std::exception&) {
        saved = common::Result<void>::failure({"RISK_AUDIT", "Risk audit sink threw an exception"});
    }
    if (!saved) {
        if (decision.accepted && !decision.duplicate_retry) undo_accept(intent, snapshot.now_ns);
        return rejected("AUDIT_UNAVAILABLE");
    }
    return decision;
}

void RiskEngine::undo_accept(const OrderIntent& intent, const std::int64_t time_ns) {
    std::lock_guard lock(mutex_);
    reservations_.erase(intent.client_order_id);
    idempotency_.erase(intent.client_order_id);
    auto found = order_times_.find(intent.account_id);
    if (found != order_times_.end()) {
        const auto latest = std::find(found->second.rbegin(), found->second.rend(), time_ns);
        if (latest != found->second.rend()) found->second.erase(std::next(latest).base());
    }
}

std::string RiskEngine::signature(const OrderIntent& intent) const {
    std::ostringstream out;
    out << intent.account_id << '|' << intent.instrument_id << '|' << intent.strategy_id << '|'
        << intent.order_id << '|'
        << static_cast<unsigned>(intent.side) << '|' << static_cast<unsigned>(intent.type) << '|'
        << intent.quantity << '|' << intent.estimated_fee_micro << '|' << intent.limit_price_micro.value_or(0) << '|'
        << intent.tick_size_micro << '|' << intent.lot_size << '|' << intent.created_at_ns;
    return out.str();
}

RiskDecision RiskEngine::evaluate_unlogged(const OrderIntent& intent, const RiskSnapshot& snapshot,
                                           const RiskLimits& limits) {
    std::lock_guard lock(mutex_);
    if (intent.client_order_id.empty() || intent.account_id == 0 || intent.instrument_id == 0 ||
        intent.tick_size_micro <= 0 || intent.lot_size == 0)
        return rejected("INVALID_INSTRUMENT");
    if (limits.max_order_quantity == 0 || limits.max_position_quantity < 0 ||
        limits.max_instrument_position_quantity < 0 || limits.max_order_notional_micro < 0 || limits.max_gross_exposure_micro < 0 ||
        limits.max_net_exposure_micro < 0 || limits.max_concentration_bps > 10'000U ||
        limits.max_daily_loss_micro < 0 || limits.max_drawdown_micro < 0 ||
        limits.max_instrument_notional_micro < 0 || limits.max_strategy_exposure_micro < 0 ||
        limits.max_strategy_daily_loss_micro < 0 || snapshot.gross_exposure_micro < 0 ||
        snapshot.instrument_exposure_micro < 0 || snapshot.strategy_exposure_micro < 0 ||
        snapshot.open_buy_reservation_quantity < 0 || snapshot.open_sell_reservation_quantity < 0 ||
        snapshot.cash_available_micro < 0 || snapshot.cash_reserved_micro < 0 || intent.estimated_fee_micro < 0)
        return rejected("RISK_CALCULATION_ERROR");
    if (intent.type == OrderType::Limit && (!intent.limit_price_micro || *intent.limit_price_micro <= 0))
        return rejected("INVALID_TICK");
    const auto request_signature = signature(intent);
    const auto prior_id = idempotency_.find(intent.client_order_id);
    if (prior_id != idempotency_.end()) {
        if (prior_id->second != request_signature) return rejected("DUPLICATE_ORDER");
        // An idempotent retry must never report acceptance when the original
        // order would now fail a time-varying safety gate. The prior acceptance
        // and its reservation are left untouched; only this retry is refused.
        auto refused = [&](const std::string_view code, const std::string_view rule,
                           const std::int64_t observed, const std::int64_t limit) {
            RiskDecision decision = rejected(std::string(code));
            decision.checks.push_back({std::string(rule), std::string(code), false, observed, limit});
            return decision;
        };
        if (kill_switch_.active(intent.account_id, intent.strategy_id))
            return refused("KILL_SWITCH_ACTIVE", "GLOBAL_ACCOUNT_STRATEGY_LATCH", 1, 0);
        if (!snapshot.data_quality_ok || snapshot.data_age_ns > limits.max_stale_age_ns)
            return refused("STALE_DATA", "MAX_DATA_AGE",
                           static_cast<std::int64_t>(std::min<std::uint64_t>(snapshot.data_age_ns, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))),
                           static_cast<std::int64_t>(std::min<std::uint64_t>(limits.max_stale_age_ns, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))));
        if (!snapshot.market_open) return refused("MARKET_CLOSED", "MARKET_SESSION", 0, 1);
        RiskDecision replay;
        replay.accepted = true;
        replay.duplicate_retry = true;
        replay.code = "IDEMPOTENT_REPLAY";
        replay.reference_price_micro = intent.limit_price_micro.value_or(
            intent.side == Side::Buy ? snapshot.ask_micro.value_or(0) : snapshot.bid_micro.value_or(0));
        const auto replay_notional = notional(replay.reference_price_micro, intent.quantity);
        if (replay_notional) replay.order_notional_micro = replay_notional.value();
        return replay;
    }

    auto decision = RiskDecision{};
    auto fail = [&](const std::string_view code, const std::string_view rule,
                    const std::int64_t observed, const std::int64_t limit) {
        decision.checks.push_back({std::string(rule), std::string(code), false, observed, limit});
        decision.code = code;
        return decision;
    };
    auto pass = [&](const std::string_view code, const std::string_view rule,
                    const std::int64_t observed, const std::int64_t limit) {
        decision.checks.push_back({std::string(rule), std::string(code), true, observed, limit});
    };

    const auto reference_price = intent.limit_price_micro.value_or(
        intent.side == Side::Buy ? snapshot.ask_micro.value_or(0) : snapshot.bid_micro.value_or(0));
    if (reference_price <= 0) return rejected("DATA_UNAVAILABLE");
    if (reference_price % intent.tick_size_micro != 0) return rejected("INVALID_TICK");
    decision.reference_price_micro = reference_price;
    const auto order_notional = notional(reference_price, intent.quantity);
    if (!order_notional) return rejected(order_notional.error().code == "ARITHMETIC_OVERFLOW"
                                          ? "RISK_CALCULATION_ERROR" : order_notional.error().code);
    decision.order_notional_micro = order_notional.value();
    if (intent.quantity == 0) return rejected("ORDER_QUANTITY_LIMIT");
    if (decision.order_notional_micro > limits.max_order_notional_micro)
        return fail("ORDER_NOTIONAL_LIMIT", "MAX_ORDER_NOTIONAL", decision.order_notional_micro, limits.max_order_notional_micro);
    pass("ORDER_NOTIONAL_LIMIT", "MAX_ORDER_NOTIONAL", decision.order_notional_micro, limits.max_order_notional_micro);

    const auto signed_qty = signed_quantity(intent.quantity);
    if (!signed_qty) return rejected(signed_qty.error().code);
    if (intent.quantity > limits.max_order_quantity || intent.quantity % intent.lot_size != 0)
        return fail("ORDER_QUANTITY_LIMIT", "MAX_ORDER_QUANTITY_AND_LOT", static_cast<std::int64_t>(std::min<common::Quantity>(intent.quantity, static_cast<common::Quantity>(std::numeric_limits<std::int64_t>::max()))), static_cast<std::int64_t>(std::min<common::Quantity>(limits.max_order_quantity, static_cast<common::Quantity>(std::numeric_limits<std::int64_t>::max()))));
    pass("ORDER_QUANTITY_LIMIT", "MAX_ORDER_QUANTITY_AND_LOT", signed_qty.value(), static_cast<std::int64_t>(std::min<common::Quantity>(limits.max_order_quantity, static_cast<common::Quantity>(std::numeric_limits<std::int64_t>::max()))));

    std::int64_t reserved_position = 0;
    std::int64_t pending_account_gross = 0;
    std::int64_t pending_account_net = 0;
    std::int64_t pending_instrument_notional = 0;
    std::int64_t pending_strategy_notional = 0;
    std::int64_t pending_buy_cash = 0;
    std::size_t pending_account_orders = 0;
    std::size_t pending_strategy_orders = 0;
    for (const auto& [unused, reservation] : reservations_) {
        (void)unused;
        if (reservation.intent.account_id != intent.account_id) continue;
        ++pending_account_orders;
        const auto gross = common::checked_add(pending_account_gross, reservation.notional_micro);
        const auto net = common::checked_add(pending_account_net, reservation.signed_notional_micro);
        if (!gross || !net) return rejected("RISK_CALCULATION_ERROR");
        pending_account_gross = gross.value();
        pending_account_net = net.value();
        if (reservation.intent.side == Side::Buy) {
            const auto cost = common::checked_add(reservation.notional_micro,
                                                  reservation.intent.estimated_fee_micro);
            if (!cost) return rejected("RISK_CALCULATION_ERROR");
            const auto pending = common::checked_add(pending_buy_cash, cost.value());
            if (!pending) return rejected("RISK_CALCULATION_ERROR");
            pending_buy_cash = pending.value();
        }
        if (reservation.intent.instrument_id == intent.instrument_id) {
            const auto position = common::checked_add(reserved_position,
                reservation.intent.side == Side::Buy ? static_cast<std::int64_t>(reservation.intent.quantity)
                                                     : -static_cast<std::int64_t>(reservation.intent.quantity));
            const auto instrument_notional = common::checked_add(pending_instrument_notional, reservation.notional_micro);
            if (!position || !instrument_notional) return rejected("RISK_CALCULATION_ERROR");
            reserved_position = position.value();
            pending_instrument_notional = instrument_notional.value();
        }
        if (reservation.intent.strategy_id == intent.strategy_id && intent.strategy_id != 0) {
            ++pending_strategy_orders;
            const auto exposure = common::checked_add(pending_strategy_notional, reservation.notional_micro);
            if (!exposure) return rejected("RISK_CALCULATION_ERROR");
            pending_strategy_notional = exposure.value();
        }
    }
    if (intent.side == Side::Buy) {
        const auto order_cost = common::checked_add(decision.order_notional_micro, intent.estimated_fee_micro);
        const auto reserved_cash = common::checked_add(snapshot.cash_reserved_micro, pending_buy_cash);
        if (!order_cost || !reserved_cash) return rejected("RISK_CALCULATION_ERROR");
        const auto available_cash = subtract(snapshot.cash_available_micro, reserved_cash.value());
        if (!available_cash) return rejected("RISK_CALCULATION_ERROR");
        if (order_cost.value() > available_cash.value())
            return fail("INSUFFICIENT_CASH", "AVAILABLE_CASH_AFTER_RESERVATIONS", order_cost.value(), available_cash.value());
    }
    const auto order_signed = intent.side == Side::Buy ? signed_qty.value() : -signed_qty.value();
    auto projected_position = common::checked_add(snapshot.position_quantity, snapshot.open_buy_reservation_quantity);
    if (!projected_position) return rejected("RISK_CALCULATION_ERROR");
    projected_position = common::checked_add(projected_position.value(), -snapshot.open_sell_reservation_quantity);
    if (!projected_position) return rejected("RISK_CALCULATION_ERROR");
    projected_position = common::checked_add(projected_position.value(), reserved_position);
    if (!projected_position) return rejected("RISK_CALCULATION_ERROR");
    projected_position = common::checked_add(projected_position.value(), order_signed);
    if (!projected_position) return rejected("RISK_CALCULATION_ERROR");
    if (!snapshot.allow_short && projected_position.value() < 0)
        return rejected("INSUFFICIENT_POSITION");
    const auto position_abs = magnitude(projected_position.value());
    if (!position_abs) return rejected(position_abs.error().code);
    if (position_abs.value() > limits.max_position_quantity)
        return fail("POSITION_LIMIT", "MAX_POSITION_SIZE", position_abs.value(), limits.max_position_quantity);
    pass("POSITION_LIMIT", "MAX_POSITION_SIZE", position_abs.value(), limits.max_position_quantity);

    auto projected_gross = common::checked_add(snapshot.gross_exposure_micro, pending_account_gross);
    if (!projected_gross) return rejected("RISK_CALCULATION_ERROR");
    projected_gross = common::checked_add(projected_gross.value(), decision.order_notional_micro);
    if (!projected_gross) return rejected("RISK_CALCULATION_ERROR");
    if (projected_gross.value() > limits.max_gross_exposure_micro)
        return fail("GROSS_EXPOSURE_LIMIT", "MAX_GROSS_EXPOSURE", projected_gross.value(), limits.max_gross_exposure_micro);
    pass("GROSS_EXPOSURE_LIMIT", "MAX_GROSS_EXPOSURE", projected_gross.value(), limits.max_gross_exposure_micro);

    const auto signed_notional = intent.side == Side::Buy ? decision.order_notional_micro : -decision.order_notional_micro;
    auto projected_net = common::checked_add(snapshot.net_exposure_micro, pending_account_net);
    if (!projected_net) return rejected("RISK_CALCULATION_ERROR");
    projected_net = common::checked_add(projected_net.value(), signed_notional);
    if (!projected_net) return rejected("RISK_CALCULATION_ERROR");
    const auto net_abs = magnitude(projected_net.value());
    if (!net_abs) return rejected(net_abs.error().code);
    if (net_abs.value() > limits.max_net_exposure_micro)
        return fail("NET_EXPOSURE_LIMIT", "MAX_NET_EXPOSURE", net_abs.value(), limits.max_net_exposure_micro);
    pass("NET_EXPOSURE_LIMIT", "MAX_NET_EXPOSURE", net_abs.value(), limits.max_net_exposure_micro);

    auto instrument_gross = common::checked_add(snapshot.instrument_exposure_micro, pending_instrument_notional);
    if (!instrument_gross) return rejected("RISK_CALCULATION_ERROR");
    instrument_gross = common::checked_add(instrument_gross.value(), decision.order_notional_micro);
    if (!instrument_gross) return rejected("RISK_CALCULATION_ERROR");
    if (exceeds_fraction(instrument_gross.value(), projected_gross.value(), limits.max_concentration_bps))
        return fail("CONCENTRATION_LIMIT", "MAX_CONCENTRATION_BPS", instrument_gross.value(), static_cast<std::int64_t>(limits.max_concentration_bps));
    pass("CONCENTRATION_LIMIT", "MAX_CONCENTRATION_BPS", instrument_gross.value(), static_cast<std::int64_t>(limits.max_concentration_bps));

    const auto daily_loss = nonnegative_loss(snapshot.daily_pnl_micro);
    if (!daily_loss) return rejected(daily_loss.error().code);
    if (daily_loss.value() > limits.max_daily_loss_micro)
        return fail("DAILY_LOSS_LIMIT", "MAX_DAILY_LOSS", daily_loss.value(), limits.max_daily_loss_micro);
    pass("DAILY_LOSS_LIMIT", "MAX_DAILY_LOSS", daily_loss.value(), limits.max_daily_loss_micro);

    auto drawdown = common::Result<std::int64_t>::success(0);
    if (snapshot.high_water_mark_micro > snapshot.equity_micro)
        drawdown = subtract(snapshot.high_water_mark_micro, snapshot.equity_micro);
    if (!drawdown) return rejected(drawdown.error().code);
    if (drawdown.value() > limits.max_drawdown_micro)
        return fail("DRAWDOWN_LIMIT", "MAX_DRAWDOWN", drawdown.value(), limits.max_drawdown_micro);
    pass("DRAWDOWN_LIMIT", "MAX_DRAWDOWN", drawdown.value(), limits.max_drawdown_micro);

    if (snapshot.open_orders > limits.max_open_orders || pending_account_orders >= limits.max_open_orders - snapshot.open_orders)
        return fail("OPEN_ORDER_LIMIT", "MAX_OPEN_ORDERS", static_cast<std::int64_t>(limits.max_open_orders + 1U), static_cast<std::int64_t>(limits.max_open_orders));
    const auto projected_open_orders = snapshot.open_orders + pending_account_orders + 1U;
    if (projected_open_orders > limits.max_open_orders)
        return fail("OPEN_ORDER_LIMIT", "MAX_OPEN_ORDERS", static_cast<std::int64_t>(projected_open_orders), static_cast<std::int64_t>(limits.max_open_orders));
    pass("OPEN_ORDER_LIMIT", "MAX_OPEN_ORDERS", static_cast<std::int64_t>(projected_open_orders), static_cast<std::int64_t>(limits.max_open_orders));

    if (snapshot.now_ns <= 0 || limits.order_rate_window_ns > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return rejected("DATA_UNAVAILABLE");
    const auto window_ns = static_cast<std::int64_t>(limits.order_rate_window_ns);
    const auto earliest = snapshot.now_ns < std::numeric_limits<std::int64_t>::min() + window_ns
        ? std::numeric_limits<std::int64_t>::min() : snapshot.now_ns - window_ns;
    std::size_t order_rate = 1;
    for (const auto time : snapshot.recent_order_times_ns) if (time >= earliest && time <= snapshot.now_ns) ++order_rate;
    auto& recent = order_times_[intent.account_id];
    recent.erase(std::remove_if(recent.begin(), recent.end(), [earliest](const auto time) { return time < earliest; }), recent.end());
    for (const auto time : recent) {
        if (time > snapshot.now_ns) return rejected("DATA_UNAVAILABLE");
        if (time >= earliest) ++order_rate;
    }
    for (const auto time : snapshot.recent_order_times_ns) if (time > snapshot.now_ns)
        return rejected("DATA_UNAVAILABLE");
    if (order_rate > limits.max_orders_per_window)
        return fail("ORDER_RATE_LIMIT", "MAX_ORDERS_PER_WINDOW", static_cast<std::int64_t>(order_rate), static_cast<std::int64_t>(limits.max_orders_per_window));
    pass("ORDER_RATE_LIMIT", "MAX_ORDERS_PER_WINDOW", static_cast<std::int64_t>(order_rate), static_cast<std::int64_t>(limits.max_orders_per_window));

    if (!snapshot.instrument_enabled || position_abs.value() > limits.max_instrument_position_quantity ||
        instrument_gross.value() > limits.max_instrument_notional_micro)
        return fail("INSTRUMENT_LIMIT", "INSTRUMENT_ALLOWLIST_AND_BUDGET", std::max(position_abs.value(), instrument_gross.value()), limits.max_instrument_notional_micro);
    pass("INSTRUMENT_LIMIT", "INSTRUMENT_ALLOWLIST_AND_BUDGET", instrument_gross.value(), limits.max_instrument_notional_micro);

    auto strategy_exposure = common::checked_add(snapshot.strategy_exposure_micro, pending_strategy_notional);
    if (!strategy_exposure) return rejected("RISK_CALCULATION_ERROR");
    strategy_exposure = common::checked_add(strategy_exposure.value(), decision.order_notional_micro);
    if (!strategy_exposure) return rejected("RISK_CALCULATION_ERROR");
    if (snapshot.strategy_orders > limits.max_strategy_orders ||
        pending_strategy_orders >= limits.max_strategy_orders - snapshot.strategy_orders)
        return fail("STRATEGY_LIMIT", "STRATEGY_ORDER_EXPOSURE_AND_LOSS_BUDGET", static_cast<std::int64_t>(limits.max_strategy_orders + 1U), static_cast<std::int64_t>(limits.max_strategy_orders));
    const auto strategy_orders = snapshot.strategy_orders + pending_strategy_orders + 1U;
    const auto strategy_loss = nonnegative_loss(snapshot.strategy_daily_pnl_micro);
    if (!strategy_loss) return rejected(strategy_loss.error().code);
    if (!snapshot.strategy_enabled || strategy_orders > limits.max_strategy_orders ||
        strategy_exposure.value() > limits.max_strategy_exposure_micro ||
        strategy_loss.value() > limits.max_strategy_daily_loss_micro)
        return fail("STRATEGY_LIMIT", "STRATEGY_ORDER_EXPOSURE_AND_LOSS_BUDGET", strategy_exposure.value(), limits.max_strategy_exposure_micro);
    pass("STRATEGY_LIMIT", "STRATEGY_ORDER_EXPOSURE_AND_LOSS_BUDGET", strategy_exposure.value(), limits.max_strategy_exposure_micro);

    if (!snapshot.data_quality_ok || snapshot.data_age_ns > limits.max_stale_age_ns)
        return fail("STALE_DATA", "MAX_DATA_AGE", static_cast<std::int64_t>(std::min<std::uint64_t>(snapshot.data_age_ns, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))), static_cast<std::int64_t>(std::min<std::uint64_t>(limits.max_stale_age_ns, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))));
    pass("STALE_DATA", "MAX_DATA_AGE", static_cast<std::int64_t>(snapshot.data_age_ns), static_cast<std::int64_t>(limits.max_stale_age_ns));

    if (!snapshot.market_open) return fail("MARKET_CLOSED", "MARKET_SESSION", 0, 1);
    pass("MARKET_CLOSED", "MARKET_SESSION", 1, 1);
    if (idempotency_.contains(intent.client_order_id)) return fail("DUPLICATE_ORDER", "CLIENT_ORDER_ID", 1, 0);
    pass("DUPLICATE_ORDER", "CLIENT_ORDER_ID", 0, 0);
    if (kill_switch_.active(intent.account_id, intent.strategy_id))
        return fail("KILL_SWITCH_ACTIVE", "GLOBAL_ACCOUNT_STRATEGY_LATCH", 1, 0);
    pass("KILL_SWITCH_ACTIVE", "GLOBAL_ACCOUNT_STRATEGY_LATCH", 0, 0);

    const auto signed_order_notional = intent.side == Side::Buy ? decision.order_notional_micro : -decision.order_notional_micro;
    reservations_.emplace(intent.client_order_id,
                          Reservation{intent, decision.order_notional_micro, signed_order_notional, request_signature});
    idempotency_.emplace(intent.client_order_id, request_signature);
    recent.push_back(snapshot.now_ns);
    decision.accepted = true;
    decision.code = "ACCEPTED";
    return decision;
}

void RiskEngine::complete(const std::string_view client_order_id) {
    std::lock_guard lock(mutex_);
    reservations_.erase(std::string(client_order_id));
}

std::size_t RiskEngine::reservation_count() const {
    std::lock_guard lock(mutex_);
    return reservations_.size();
}

} // namespace basalt::trading
