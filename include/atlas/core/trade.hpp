#pragma once

#include "atlas/core/order.hpp"

#include <string>
#include <vector>

namespace atlas {

struct Trade {
    TradeId trade_id{0};
    OrderId buy_order_id{0};
    OrderId sell_order_id{0};
    OrderId aggressor_order_id{0};
    OrderId resting_order_id{0};
    Side aggressor_side{Side::Buy};
    Price price{0};
    Quantity quantity{0};
    SequenceNumber sequence{0};
    TimestampNs timestamp_ns{0};
};

struct ActivityRecord {
    SequenceNumber sequence{0};
    ActionType action{ActionType::Submit};
    OrderId order_id{0};
    Side side{Side::Buy};
    OrderType order_type{OrderType::Limit};
    Price price{0};
    Quantity quantity{0};
    OrderStatus status{OrderStatus::New};
    std::string message{};
};

struct OrderResult {
    OrderId order_id{0};
    OrderStatus status{OrderStatus::Rejected};
    Quantity initial_quantity{0};
    Quantity filled_quantity{0};
    Quantity remaining_quantity{0};
    SequenceNumber sequence{0};
    std::vector<Trade> trades{};
    std::string message{};
};

struct CancelResult {
    OrderId order_id{0};
    bool success{false};
    OrderStatus status{OrderStatus::Rejected};
    Price cancelled_price{0};
    Quantity cancelled_quantity{0};
    SequenceNumber sequence{0};
    std::string message{};
};

struct PriceLevelSummary {
    Price price{0};
    Quantity total_volume{0};
    std::size_t order_count{0};
};

struct BookSnapshot {
    std::vector<PriceLevelSummary> bids{};
    std::vector<PriceLevelSummary> asks{};
    std::vector<Order> resting_orders{};
    bool has_best_bid{false};
    Price best_bid{0};
    Quantity best_bid_volume{0};
    bool has_best_ask{false};
    Price best_ask{0};
    Quantity best_ask_volume{0};
    bool has_spread{false};
    Price spread{0};
};

struct EngineStats {
    std::uint64_t total_orders_submitted{0};
    std::uint64_t total_orders_rejected{0};
    std::uint64_t total_orders_cancelled{0};
    std::uint64_t total_trades_executed{0};
    Quantity total_volume_traded{0};
    bool has_last_trade_price{false};
    Price last_trade_price{0};
    Quantity last_trade_quantity{0};
    std::size_t active_order_count{0};
    std::size_t bid_level_count{0};
    std::size_t ask_level_count{0};
};

} // namespace atlas
