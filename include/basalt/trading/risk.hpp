#pragma once

#include "basalt/persistence/event_log.hpp"
#include "basalt/trading/types.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::trading {

enum class KillScope : std::uint8_t { Global, Account, Strategy };

struct KillSwitchEvent {
    std::uint64_t sequence{};
    KillScope scope{KillScope::Global};
    std::uint64_t target_id{};
    bool active{};
    std::string actor{};
    std::string reason{};
    std::int64_t time_ns{};
    bool working_orders_cancelled{};
    bool persisted{};
    friend bool operator==(const KillSwitchEvent&, const KillSwitchEvent&) = default;
};

class KillSwitch {
public:
    using EventSink = std::function<common::Result<void>(const KillSwitchEvent&)>;
    using CancelSink = std::function<common::Result<void>(KillScope, std::uint64_t)>;

    explicit KillSwitch(EventSink sink = {}, CancelSink cancel_sink = {});
    [[nodiscard]] static EventSink event_log_sink(persistence::EventLog& log);
    [[nodiscard]] common::Result<void> restore(const std::vector<persistence::LogFrame>& frames);
    [[nodiscard]] KillSwitchEvent activate(KillScope scope, std::uint64_t target_id,
                                           std::string actor, std::string reason,
                                           std::int64_t time_ns);
    [[nodiscard]] common::Result<KillSwitchEvent> reset(KillScope scope,
                                                         std::uint64_t target_id,
                                                         std::string actor,
                                                         std::string reason,
                                                         std::int64_t time_ns,
                                                         bool authorized,
                                                         bool health_checks_passed);
    [[nodiscard]] bool active(common::AccountId account_id,
                              common::StrategyId strategy_id = 0) const;
    [[nodiscard]] std::vector<KillSwitchEvent> events() const;

private:
    using Key = std::pair<KillScope, std::uint64_t>;
    [[nodiscard]] common::Result<KillSwitchEvent> record(KillSwitchEvent event);
    mutable std::mutex mutex_;
    std::map<Key, bool> states_{};
    std::map<Key, std::uint64_t> state_sequences_{};
    std::vector<KillSwitchEvent> events_{};
    std::uint64_t next_sequence_{1};
    EventSink sink_{};
    CancelSink cancel_sink_{};
};

struct RiskLimits {
    std::int64_t max_order_notional_micro{10'000'000'000LL};
    common::Quantity max_order_quantity{10'000};
    common::PositionQuantity max_position_quantity{10'000};
    std::int64_t max_gross_exposure_micro{100'000'000'000LL};
    std::int64_t max_net_exposure_micro{50'000'000'000LL};
    std::uint32_t max_concentration_bps{10'000};
    std::int64_t max_daily_loss_micro{1'000'000'000LL};
    std::int64_t max_drawdown_micro{1'000'000'000LL};
    std::size_t max_open_orders{50};
    std::size_t max_orders_per_window{10};
    std::uint64_t order_rate_window_ns{1'000'000'000ULL};
    std::uint64_t max_stale_age_ns{5'000'000'000ULL};
    common::PositionQuantity max_instrument_position_quantity{10'000};
    std::int64_t max_instrument_notional_micro{10'000'000'000LL};
    std::size_t max_strategy_orders{50};
    std::int64_t max_strategy_exposure_micro{10'000'000'000LL};
    std::int64_t max_strategy_daily_loss_micro{500'000'000LL};
};

struct RiskSnapshot {
    common::PositionQuantity position_quantity{};
    common::PositionQuantity open_buy_reservation_quantity{};
    common::PositionQuantity open_sell_reservation_quantity{};
    std::int64_t gross_exposure_micro{};
    std::int64_t net_exposure_micro{};
    std::int64_t instrument_exposure_micro{};
    std::int64_t daily_pnl_micro{};
    std::int64_t equity_micro{};
    std::int64_t high_water_mark_micro{};
    std::int64_t strategy_exposure_micro{};
    std::int64_t strategy_daily_pnl_micro{};
    std::int64_t cash_available_micro{};
    std::int64_t cash_reserved_micro{};
    std::size_t open_orders{};
    std::size_t strategy_orders{};
    std::uint64_t data_age_ns{};
    std::int64_t now_ns{};
    std::vector<std::int64_t> recent_order_times_ns{};
    std::optional<common::MicroUnits> bid_micro{};
    std::optional<common::MicroUnits> ask_micro{};
    bool instrument_enabled{true};
    bool strategy_enabled{true};
    bool data_quality_ok{true};
    bool market_open{true};
    bool allow_short{};
};

struct RiskCheck {
    std::string rule{};
    std::string rejection_code{};
    bool passed{};
    std::int64_t observed{};
    std::int64_t limit{};
};

struct RiskDecision {
    bool accepted{};
    bool duplicate_retry{};
    std::string code{};
    common::MicroUnits reference_price_micro{};
    std::int64_t order_notional_micro{};
    std::vector<RiskCheck> checks{};
};

class RiskEngine {
public:
    using EventSink = std::function<common::Result<void>(const std::string&)>;
    explicit RiskEngine(KillSwitch& kill_switch, EventSink sink = {})
        : kill_switch_(kill_switch), sink_(std::move(sink)) {}
    [[nodiscard]] static EventSink event_log_sink(persistence::EventLog& log);
    [[nodiscard]] common::Result<void> restore(const std::vector<persistence::LogFrame>& frames);
    [[nodiscard]] RiskDecision evaluate(const OrderIntent& intent,
                                        const RiskSnapshot& snapshot,
                                        const RiskLimits& limits = {});
    void complete(std::string_view client_order_id);
    [[nodiscard]] std::size_t reservation_count() const;

private:
    struct Reservation {
        OrderIntent intent{};
        std::int64_t notional_micro{};
        std::int64_t signed_notional_micro{};
        std::string signature{};
    };
    [[nodiscard]] std::string signature(const OrderIntent& intent) const;
    [[nodiscard]] RiskDecision evaluate_unlogged(const OrderIntent& intent,
                                                 const RiskSnapshot& snapshot,
                                                 const RiskLimits& limits);
    void undo_accept(const OrderIntent& intent, std::int64_t time_ns);
    KillSwitch& kill_switch_;
    EventSink sink_{};
    std::mutex evaluation_mutex_{};
    mutable std::mutex mutex_;
    std::map<std::string, Reservation, std::less<>> reservations_{};
    std::map<std::string, std::string, std::less<>> idempotency_{};
    std::map<common::AccountId, std::vector<std::int64_t>> order_times_{};
};

} // namespace basalt::trading
