#include "basalt/core/vector_order_book.hpp"

#include <algorithm>
#include <limits>

namespace basalt {
namespace {

bool fail(std::string* error_out, const std::string& message) {
    if (error_out != nullptr) *error_out = message;
    return false;
}

template <typename Levels>
void append_summaries(const Levels& levels, const std::size_t depth,
                      std::vector<PriceLevelSummary>& output) {
    output.reserve(std::min(depth, levels.size()));
    for (const auto& level : levels) {
        if (output.size() == depth) break;
        output.push_back(PriceLevelSummary{level.price, level.total_volume, level.orders.size()});
    }
}

template <typename Levels>
void append_orders(const Levels& levels, std::vector<Order>& output) {
    for (const auto& level : levels) {
        output.insert(output.end(), level.orders.begin(), level.orders.end());
    }
}

} // namespace

std::vector<VectorPriceLevel>& VectorOrderBook::levels_for(const Side side) noexcept {
    return side == Side::Buy ? bids_ : asks_;
}

const std::vector<VectorPriceLevel>& VectorOrderBook::levels_for(const Side side) const noexcept {
    return side == Side::Buy ? bids_ : asks_;
}

bool VectorOrderBook::has_order_in(const std::vector<VectorPriceLevel>& levels,
                                   const OrderId id) const noexcept {
    for (const auto& level : levels) {
        if (std::any_of(level.orders.begin(), level.orders.end(),
                        [id](const Order& order) { return order.id == id; })) return true;
    }
    return false;
}

const Order* VectorOrderBook::find_order_in(const std::vector<VectorPriceLevel>& levels,
                                            const OrderId id) const noexcept {
    for (const auto& level : levels) {
        const auto found = std::find_if(level.orders.begin(), level.orders.end(),
                                        [id](const Order& order) { return order.id == id; });
        if (found != level.orders.end()) return &*found;
    }
    return nullptr;
}

bool VectorOrderBook::has_order(const OrderId id) const noexcept {
    return has_order_in(bids_, id) || has_order_in(asks_, id);
}

const Order* VectorOrderBook::get_order(const OrderId id) const noexcept {
    if (const Order* bid = find_order_in(bids_, id); bid != nullptr) return bid;
    return find_order_in(asks_, id);
}

std::size_t VectorOrderBook::active_order_count() const noexcept {
    std::size_t count = 0;
    for (const auto& level : bids_) count += level.orders.size();
    for (const auto& level : asks_) count += level.orders.size();
    return count;
}

bool VectorOrderBook::insert_into(std::vector<VectorPriceLevel>& levels, const Order& order,
                                  const bool descending) {
    const auto position = std::lower_bound(
        levels.begin(), levels.end(), order.price,
        [descending](const VectorPriceLevel& level, const Price price) {
            return descending ? level.price > price : level.price < price;
        });
    if (position != levels.end() && position->price == order.price) {
        if (std::numeric_limits<Quantity>::max() - position->total_volume < order.remaining_quantity) {
            return false;
        }
        position->orders.push_back(order);
        position->total_volume += order.remaining_quantity;
        return true;
    }

    VectorPriceLevel new_level{};
    new_level.price = order.price;
    new_level.total_volume = order.remaining_quantity;
    new_level.orders.push_back(order);
    levels.insert(position, std::move(new_level));
    return true;
}

bool VectorOrderBook::insert_resting_order(const Order& order) {
    if (order.id == 0 || order.type != OrderType::Limit || order.price <= 0 ||
        (order.side != Side::Buy && order.side != Side::Sell) ||
        order.remaining_quantity == 0 || order.remaining_quantity > order.initial_quantity ||
        has_order(order.id)) return false;
    if ((order.side == Side::Buy && !asks_.empty() && order.price >= asks_.front().price) ||
        (order.side == Side::Sell && !bids_.empty() && order.price <= bids_.front().price)) return false;
    return insert_into(levels_for(order.side), order, order.side == Side::Buy);
}

bool VectorOrderBook::cancel_order(const OrderId id) {
    for (auto* levels : {&bids_, &asks_}) {
        for (auto level_it = levels->begin(); level_it != levels->end(); ++level_it) {
            const auto order_it = std::find_if(level_it->orders.begin(), level_it->orders.end(),
                                               [id](const Order& order) { return order.id == id; });
            if (order_it == level_it->orders.end()) continue;
            level_it->total_volume -= order_it->remaining_quantity;
            level_it->orders.erase(order_it);
            if (level_it->orders.empty()) levels->erase(level_it);
            return true;
        }
    }
    return false;
}

OrderStatus VectorOrderBook::submit_order(Order incoming, SequenceNumber& sequence_counter,
                                          TradeId& trade_id_counter,
                                          std::vector<Trade>& out_trades) {
    while (incoming.remaining_quantity > 0) {
        const bool buying = incoming.side == Side::Buy;
        auto& opposing = buying ? asks_ : bids_;
        if (opposing.empty()) break;
        VectorPriceLevel& level = opposing.front();
        if (incoming.type == OrderType::Limit &&
            ((buying && incoming.price < level.price) || (!buying && incoming.price > level.price))) break;

        while (incoming.remaining_quantity > 0 && !level.orders.empty()) {
            Order& resting = level.orders.front();
            const Quantity filled = std::min(incoming.remaining_quantity, resting.remaining_quantity);
            incoming.remaining_quantity -= filled;
            resting.remaining_quantity -= filled;
            level.total_volume -= filled;

            Trade trade{};
            trade.trade_id = ++trade_id_counter;
            trade.buy_order_id = buying ? incoming.id : resting.id;
            trade.sell_order_id = buying ? resting.id : incoming.id;
            trade.aggressor_order_id = incoming.id;
            trade.resting_order_id = resting.id;
            trade.aggressor_side = incoming.side;
            trade.price = resting.price;
            trade.quantity = filled;
            trade.sequence = ++sequence_counter;
            trade.timestamp_ns = incoming.timestamp_ns;
            out_trades.push_back(trade);
            if (resting.remaining_quantity == 0) level.orders.erase(level.orders.begin());
        }
        if (level.orders.empty()) opposing.erase(opposing.begin());
    }

    if (incoming.remaining_quantity == 0) return OrderStatus::Filled;
    if (incoming.type == OrderType::Market) {
        return incoming.remaining_quantity < incoming.initial_quantity
            ? OrderStatus::PartiallyFilled : OrderStatus::Cancelled;
    }
    return insert_resting_order(incoming)
        ? (incoming.remaining_quantity < incoming.initial_quantity
            ? OrderStatus::PartiallyFilled : OrderStatus::New)
        : OrderStatus::Rejected;
}

BookSnapshot VectorOrderBook::get_snapshot(const std::size_t depth) const {
    BookSnapshot snapshot{};
    append_summaries(bids_, depth, snapshot.bids);
    append_summaries(asks_, depth, snapshot.asks);
    if (!bids_.empty()) {
        snapshot.has_best_bid = true;
        snapshot.best_bid = bids_.front().price;
        snapshot.best_bid_volume = bids_.front().total_volume;
    }
    if (!asks_.empty()) {
        snapshot.has_best_ask = true;
        snapshot.best_ask = asks_.front().price;
        snapshot.best_ask_volume = asks_.front().total_volume;
    }
    if (snapshot.has_best_bid && snapshot.has_best_ask) {
        snapshot.has_spread = true;
        snapshot.spread = snapshot.best_ask - snapshot.best_bid;
    }
    snapshot.resting_orders.reserve(active_order_count());
    append_orders(bids_, snapshot.resting_orders);
    append_orders(asks_, snapshot.resting_orders);
    return snapshot;
}

bool VectorOrderBook::verify_invariants(std::string* error_out) const {
    if (error_out != nullptr) error_out->clear();
    std::size_t total_count = 0;
    const auto check = [&](const std::vector<VectorPriceLevel>& levels, const Side side) {
        bool first_level = true;
        Price previous_price = 0;
        for (const auto& level : levels) {
            if (!first_level &&
                ((side == Side::Buy && previous_price <= level.price) ||
                 (side == Side::Sell && previous_price >= level.price))) {
                return fail(error_out, "Vector price levels are not strictly ordered");
            }
            first_level = false;
            previous_price = level.price;
            if (level.orders.empty() || level.total_volume == 0) {
                return fail(error_out, "Vector price level is empty or has no volume");
            }
            Quantity volume = 0;
            SequenceNumber previous_sequence = 0;
            bool first_order = true;
            for (const Order& order : level.orders) {
                ++total_count;
                if (order.side != side || order.type != OrderType::Limit || order.price != level.price ||
                    order.remaining_quantity == 0 || order.remaining_quantity > order.initial_quantity) {
                    return fail(error_out, "Vector resting order has invalid level assignment or quantity");
                }
                if (!first_order && order.sequence <= previous_sequence) {
                    return fail(error_out, "Vector FIFO sequence numbers are not increasing");
                }
                first_order = false;
                previous_sequence = order.sequence;
                if (std::numeric_limits<Quantity>::max() - volume < order.remaining_quantity) {
                    return fail(error_out, "Vector price level volume sum overflows Quantity");
                }
                volume += order.remaining_quantity;
            }
            if (volume != level.total_volume) return fail(error_out, "Vector cached volume mismatch");
        }
        return true;
    };

    if (!check(bids_, Side::Buy) || !check(asks_, Side::Sell)) return false;
    if (!bids_.empty() && !asks_.empty() && bids_.front().price >= asks_.front().price) {
        return fail(error_out, "Vector resting book is crossed");
    }
    if (total_count != active_order_count()) return fail(error_out, "Vector resting order count mismatch");
    return true;
}

void VectorOrderBook::clear() noexcept {
    bids_.clear();
    asks_.clear();
}

} // namespace basalt
