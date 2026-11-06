#pragma once

#include "basalt/trading/types.hpp"
#include "basalt/persistence/event_log.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <utility>

namespace basalt::trading {

struct PositionState {
    common::PositionQuantity quantity{};
    common::MicroUnits average_cost_micro{};
    // Cost-basis remainder is retained on partial closes and assigned on full close.
    std::int64_t cost_residual{};
    std::int64_t realized_pnl_micro{};
    std::int64_t fees_micro{};
    std::uint64_t updated_sequence{};
    std::optional<common::MicroUnits> mark_micro{};
    std::string mark_source{"UNMARKED"};
    std::int64_t unrealized_pnl_micro{};
    friend bool operator==(const PositionState&, const PositionState&) = default;
};

struct LedgerAccount {
    common::AccountId account_id{};
    std::string name{};
    std::int64_t initial_cash_micro{};
    std::int64_t cash_balance_micro{};
    bool allow_short{};
};

struct ExecutionFill {
    std::uint64_t execution_id{};
    std::uint64_t event_sequence{};
    std::uint64_t order_id{};
    common::AccountId account_id{};
    common::InstrumentId instrument_id{};
    common::StrategyId strategy_id{};
    Side side{Side::Buy};
    common::MicroUnits price_micro{};
    common::Quantity quantity{};
    std::int64_t fee_micro{};
    PaperExecutionModel model{PaperExecutionModel::ConservativeQuote};
    std::uint32_t model_version{1};
    common::DataSourceType source{common::DataSourceType::Derived};
    common::FeedQualityState quality{common::FeedQualityState::Ok};
    std::string fee_schedule_id{"NO_FEES_CONFIGURED"};
    std::uint32_t fee_schedule_version{1};
    std::string fee_schedule_source{"unspecified"};
    std::int64_t time_ns{};
};

struct CashJournalEntry {
    std::uint64_t execution_id{};
    std::uint64_t event_sequence{};
    common::AccountId account_id{};
    std::int64_t debit_micro{};
    std::int64_t credit_micro{};
    std::int64_t contra_debit_micro{};
    std::int64_t contra_credit_micro{};
    std::int64_t fee_micro{};
    std::int64_t balance_micro{};
    std::string memo{};
    friend bool operator==(const CashJournalEntry&, const CashJournalEntry&) = default;
};

struct PortfolioSummary {
    common::AccountId account_id{};
    std::int64_t cash_balance_micro{};
    std::int64_t total_fees_micro{};
    std::int64_t realized_pnl_micro{};
    std::int64_t unrealized_pnl_micro{};
    std::vector<std::pair<common::InstrumentId, PositionState>> positions{};
};

class PortfolioLedger {
public:
    using EventSink = std::function<common::Result<void>(const std::string&)>;
    explicit PortfolioLedger(EventSink sink = {}) : sink_(std::move(sink)) {}
    [[nodiscard]] static EventSink event_log_sink(persistence::EventLog& log);
    [[nodiscard]] common::Result<void> restore(const std::vector<persistence::LogFrame>& frames);
    [[nodiscard]] common::Result<void> create_account(LedgerAccount account);
    [[nodiscard]] common::Result<void> apply_fill(const ExecutionFill& fill);
    [[nodiscard]] common::Result<void> mark(common::AccountId account_id,
                                             common::InstrumentId instrument_id,
                                             std::optional<common::MicroUnits> bid_micro,
                                             std::optional<common::MicroUnits> ask_micro);
    [[nodiscard]] common::Result<LedgerAccount> account(common::AccountId account_id) const;
    [[nodiscard]] common::Result<std::optional<PositionState>> position(
        common::AccountId account_id, common::InstrumentId instrument_id) const;
    [[nodiscard]] common::Result<PortfolioSummary> summary(common::AccountId account_id) const;
    [[nodiscard]] common::Result<void> verify_reconciliation() const;
    [[nodiscard]] const std::vector<CashJournalEntry>& journal() const noexcept { return journal_; }
    [[nodiscard]] const std::vector<ExecutionFill>& fills() const noexcept { return fills_; }

private:
    using PositionKey = std::pair<common::AccountId, common::InstrumentId>;
    std::map<common::AccountId, LedgerAccount> accounts_{};
    std::map<PositionKey, PositionState> positions_{};
    std::set<std::uint64_t> execution_ids_{};
    std::set<std::uint64_t> event_sequences_{};
    std::vector<CashJournalEntry> journal_{};
    std::vector<ExecutionFill> fills_{};
    EventSink sink_{};
    std::uint64_t last_event_sequence_{};
};

} // namespace basalt::trading
