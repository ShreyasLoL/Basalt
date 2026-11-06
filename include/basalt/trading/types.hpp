#pragma once

#include "basalt/common/types.hpp"
#include "basalt/market_data/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace basalt::trading {

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Market, Limit };
enum class PaperExecutionModel : std::uint8_t {
    ConservativeQuote,
    BarApproximation,
    OrderBookReplay,
    InternalClob
};

enum class FeeComponentKind : std::uint8_t {
    Brokerage,
    SecuritiesTransactionTax,
    ExchangeTransaction,
    SebiTurnover,
    StampDuty,
    Gst,
    Other
};

struct FeeComponent {
    FeeComponentKind kind{FeeComponentKind::Other};
    std::uint32_t buy_basis_points{};
    std::uint32_t sell_basis_points{};
    std::int64_t fixed_micro{};
    std::int64_t cap_micro{};
    bool gst_eligible{};
};

struct FeeSchedule {
    std::string schedule_id{"NO_FEES_CONFIGURED"};
    std::uint32_t version{1};
    std::string source{"unspecified"};
    std::string venue{"unspecified"};
    std::string segment{"unspecified"};
    std::string product{"unspecified"};
    std::int64_t valid_from_ns{};
    std::int64_t valid_to_ns{};
    std::vector<FeeComponent> components{};
    std::uint32_t gst_basis_points{};
};

struct OrderIntent {
    std::string client_order_id{};
    std::uint64_t order_id{};
    common::AccountId account_id{};
    common::InstrumentId instrument_id{};
    common::StrategyId strategy_id{};
    Side side{Side::Buy};
    OrderType type{OrderType::Market};
    common::Quantity quantity{};
    std::int64_t estimated_fee_micro{};
    std::optional<common::MicroUnits> limit_price_micro{};
    common::MicroUnits tick_size_micro{};
    common::Quantity lot_size{1};
    std::int64_t created_at_ns{};
};

struct DepthLevel {
    common::MicroUnits price_micro{};
    common::Quantity quantity{};
};

struct ExecutionMarket {
    std::uint64_t event_sequence{};
    common::DataSourceType source{common::DataSourceType::Derived};
    common::FeedQualityState quality{common::FeedQualityState::Disconnected};
    std::int64_t event_time_ns{};
    std::int64_t receive_time_ns{};
    std::int64_t now_ns{};
    bool sequence_gap{};
    std::optional<common::MicroUnits> bid_micro{};
    std::optional<common::Quantity> bid_quantity{};
    std::optional<common::MicroUnits> ask_micro{};
    std::optional<common::Quantity> ask_quantity{};
    std::vector<DepthLevel> bids{};
    std::vector<DepthLevel> asks{};
    std::optional<market_data::OhlcvBar> next_bar{};
    std::int64_t signal_bar_close_ns{};
    std::int64_t next_bar_open_ns{};
    common::MicroUnits half_spread_micro{};
};

struct ExecutionConfig {
    PaperExecutionModel model{PaperExecutionModel::ConservativeQuote};
    std::uint32_t model_version{1};
    common::MicroUnits tick_size_micro{};
    std::uint64_t slippage_ticks{};
    std::uint32_t participation_bps{10'000};
    std::uint64_t stale_after_ns{5'000'000'000ULL};
    std::uint64_t simulated_latency_ns{};
    FeeSchedule fee_schedule{};
    bool leave_unfilled_working{};
};

struct PaperFill {
    common::MicroUnits price_micro{};
    common::Quantity quantity{};
    std::int64_t fee_micro{};
};

struct ExecutionReport {
    std::string client_order_id{};
    common::AccountId account_id{};
    common::InstrumentId instrument_id{};
    common::StrategyId strategy_id{};
    Side side{Side::Buy};
    PaperExecutionModel model{PaperExecutionModel::ConservativeQuote};
    std::uint32_t model_version{};
    common::Quantity requested_quantity{};
    common::Quantity filled_quantity{};
    common::Quantity remaining_quantity{};
    std::uint64_t reference_event_sequence{};
    common::DataSourceType source{common::DataSourceType::Derived};
    common::FeedQualityState quality{common::FeedQualityState::Disconnected};
    std::uint64_t latency_ns{};
    common::MicroUnits slippage_micro{};
    std::string status{"UNFILLED"};
    std::string reason{};
    std::string fee_schedule_id{};
    std::uint32_t fee_schedule_version{};
    std::string fee_schedule_source{};
    std::vector<PaperFill> fills{};
};

} // namespace basalt::trading
