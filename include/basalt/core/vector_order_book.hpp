#pragma once

#include "basalt/core/trade.hpp"

#include <vector>

namespace basalt {

struct VectorPriceLevel {
    Price price{0};
    Quantity total_volume{0};
    std::vector<Order> orders{};
};

class VectorOrderBook {
public:
    [[nodiscard]] bool has_order(OrderId id) const noexcept;
    [[nodiscard]] const Order* get_order(OrderId id) const noexcept;
    [[nodiscard]] std::size_t active_order_count() const noexcept;
    [[nodiscard]] std::size_t bid_level_count() const noexcept { return bids_.size(); }
    [[nodiscard]] std::size_t ask_level_count() const noexcept { return asks_.size(); }

    [[nodiscard]] bool insert_resting_order(const Order& order);
    [[nodiscard]] bool cancel_order(OrderId id);
    [[nodiscard]] OrderStatus submit_order(Order incoming, SequenceNumber& sequence_counter,
                                           TradeId& trade_id_counter,
                                           std::vector<Trade>& out_trades);
    [[nodiscard]] BookSnapshot get_snapshot(std::size_t depth = 20) const;
    [[nodiscard]] bool verify_invariants(std::string* error_out = nullptr) const;
    void clear() noexcept;

private:
    std::vector<VectorPriceLevel> bids_{}; // Highest price first
    std::vector<VectorPriceLevel> asks_{}; // Lowest price first

    [[nodiscard]] std::vector<VectorPriceLevel>& levels_for(Side side) noexcept;
    [[nodiscard]] const std::vector<VectorPriceLevel>& levels_for(Side side) const noexcept;
    [[nodiscard]] bool has_order_in(const std::vector<VectorPriceLevel>& levels,
                                    OrderId id) const noexcept;
    [[nodiscard]] const Order* find_order_in(const std::vector<VectorPriceLevel>& levels,
                                             OrderId id) const noexcept;
    [[nodiscard]] bool insert_into(std::vector<VectorPriceLevel>& levels,
                                   const Order& order, bool descending);
};

} // namespace basalt
