#pragma once

#include "basalt/core/trade.hpp"
#include "basalt/common/types.hpp"

#include <deque>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace basalt {

enum class AdvancedOrderType : std::uint8_t { Limit, Market, Ioc, Fok, PostOnly, StopMarket, StopLimit };
enum class STPPolicy : std::uint8_t { CancelNewest, CancelOldest, CancelBoth };

struct AdvancedOrderRequest {
    OrderId id{};
    common::InstrumentId instrument_id{};
    Side side{Side::Buy};
    AdvancedOrderType type{AdvancedOrderType::Limit};
    Price price{};
    Price stop_price{};
    Quantity quantity{};
    std::string account_id{};
    std::string participant_id{};
    STPPolicy stp_policy{STPPolicy::CancelNewest};
};

struct InstrumentTrade {
    common::InstrumentId instrument_id{};
    Trade trade{};
    std::string buyer_account_id{};
    std::string seller_account_id{};
    std::string buyer_participant_id{};
    std::string seller_participant_id{};
};

struct AdvancedOrderResult {
    common::InstrumentId instrument_id{};
    OrderId order_id{};
    OrderStatus status{OrderStatus::Rejected};
    Quantity initial_quantity{};
    Quantity filled_quantity{};
    Quantity remaining_quantity{};
    SequenceNumber sequence{};
    std::vector<InstrumentTrade> trades{};
    std::string message{};
};

struct ModifyOrderRequest {
    common::InstrumentId instrument_id{};
    OrderId order_id{};
    Price new_price{};
    Quantity new_total_quantity{};
};

struct ModifyOrderResult {
    bool success{};
    bool priority_reset{};
    SequenceNumber sequence{};
    Quantity remaining_quantity{};
    std::string message{};
};

class MultiInstrumentEngine {
public:
    [[nodiscard]] AdvancedOrderResult submit_order(const AdvancedOrderRequest& request,
                                                    TimestampNs timestamp_ns = 0);
    [[nodiscard]] CancelResult cancel_order(common::InstrumentId instrument_id, OrderId order_id);
    [[nodiscard]] ModifyOrderResult modify_order(const ModifyOrderRequest& request);
    [[nodiscard]] common::Result<std::vector<AdvancedOrderResult>> process_reference_trade(
        common::InstrumentId instrument_id, Price reference_price, TimestampNs timestamp_ns);
    [[nodiscard]] BookSnapshot get_snapshot(common::InstrumentId instrument_id,
                                            std::size_t depth = 20) const;
    [[nodiscard]] bool verify_invariants(std::string* error_out = nullptr) const;
    [[nodiscard]] std::size_t active_order_count() const noexcept;
    [[nodiscard]] std::vector<common::InstrumentId> instruments() const;
    void reset() noexcept;

private:
    struct RestingOrder { AdvancedOrderRequest request{}; Quantity remaining_quantity{}; SequenceNumber sequence{}; };
    using OrderList = std::list<RestingOrder>;
    using BidLevels = std::map<Price, OrderList, std::greater<Price>>;
    using AskLevels = std::map<Price, OrderList, std::less<Price>>;
    struct InstrumentBook { BidLevels bids{}; AskLevels asks{}; };
    struct OrderLocation {
        common::InstrumentId instrument_id{};
        Side side{Side::Buy};
        Price price{};
        OrderList::iterator iterator{};
    };
    struct StopOrder { AdvancedOrderRequest request{}; SequenceNumber sequence{}; };

    std::map<common::InstrumentId, InstrumentBook> books_{};
    std::unordered_map<OrderId, OrderLocation> locations_{};
    std::unordered_map<OrderId, StopOrder> stops_{};
    SequenceNumber next_sequence_{};
    TradeId next_trade_id_{};
    TimestampNs logical_time_{};

    [[nodiscard]] bool would_cross(const InstrumentBook& book, Side side, Price price) const noexcept;
    [[nodiscard]] bool can_fill(const InstrumentBook& book, const AdvancedOrderRequest& request) const noexcept;
    [[nodiscard]] bool rest(InstrumentBook& book, const AdvancedOrderRequest& request,
                            SequenceNumber sequence, Quantity remaining_quantity);
    void erase_resting(OrderId id) noexcept;
    [[nodiscard]] AdvancedOrderResult submit_validated(const AdvancedOrderRequest& request,
                                                       TimestampNs timestamp_ns);
};

[[nodiscard]] std::string_view to_string(AdvancedOrderType type) noexcept;
[[nodiscard]] std::string_view to_string(STPPolicy policy) noexcept;

} // namespace basalt
