#pragma once

#include "basalt/core/trade.hpp"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace basalt {

class OrderBook {
public:
    using BidLevels = std::map<Price, PriceLevel, std::greater<Price>>;
    using AskLevels = std::map<Price, PriceLevel, std::less<Price>>;

    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;
    [[nodiscard]] std::optional<Price> spread() const noexcept;
    [[nodiscard]] bool has_order(OrderId id) const noexcept;
    [[nodiscard]] const Order* get_order(OrderId id) const noexcept;
    [[nodiscard]] std::size_t active_order_count() const noexcept;
    [[nodiscard]] std::size_t bid_level_count() const noexcept { return bids_.size(); }
    [[nodiscard]] std::size_t ask_level_count() const noexcept { return asks_.size(); }

    [[nodiscard]] bool insert_resting_order(const Order& order);
    [[nodiscard]] CancelResult cancel_order(OrderId id);
    void match_against_book(Order& incoming, SequenceNumber& seq_counter,
                            TradeId& trade_id_counter, TimestampNs now,
                            std::vector<Trade>& out_trades);
    [[nodiscard]] BookSnapshot get_snapshot(std::size_t depth = 20) const;
    void clear() noexcept;
    [[nodiscard]] bool verify_invariants(std::string* error_out = nullptr) const;

private:
    BidLevels bids_{};
    AskLevels asks_{};
    std::unordered_map<OrderId, OrderLocation> order_lookup_{};

    template <typename Levels>
    void append_level_snapshot(const Levels& levels, std::size_t depth,
                               std::vector<PriceLevelSummary>& out) const;
};

} // namespace basalt
