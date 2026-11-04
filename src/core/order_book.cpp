#include "atlas/core/order_book.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <sstream>
#include <utility>

namespace atlas {
namespace {

template <typename Levels>
std::size_t count_orders(const Levels& levels) noexcept {
    std::size_t count = 0;
    for (const auto& [price, level] : levels) {
        (void)price;
        count += level.orders.size();
    }
    return count;
}

bool fail(std::string* error_out, const std::string& message) {
    if (error_out != nullptr) {
        *error_out = message;
    }
    return false;
}

template <typename Levels>
bool check_price_map(const Levels& levels, const Side expected_side,
                     const std::unordered_map<OrderId, OrderLocation>& lookup,
                     std::string* error_out) {
    bool first = true;
    Price previous_price = 0;
    for (const auto& [price, level] : levels) {
        if (!first) {
            if (expected_side == Side::Buy && !(previous_price > price)) {
                return fail(error_out, "Bid price levels are not strictly descending");
            }
            if (expected_side == Side::Sell && !(previous_price < price)) {
                return fail(error_out, "Ask price levels are not strictly ascending");
            }
        }
        first = false;
        previous_price = price;

        if (level.price != price) {
            return fail(error_out, "Price level key does not match its stored price");
        }
        if (level.orders.empty() || level.total_volume == 0) {
            return fail(error_out, "Price level is empty or has zero volume");
        }

        Quantity summed_volume = 0;
        SequenceNumber previous_sequence = 0;
        bool first_order = true;
        for (auto it = level.orders.begin(); it != level.orders.end(); ++it) {
            const Order& order = *it;
            if (order.remaining_quantity == 0 ||
                order.remaining_quantity > order.initial_quantity) {
                return fail(error_out, "Resting order has invalid remaining quantity");
            }
            if (order.price != price || order.type != OrderType::Limit || order.side != expected_side) {
                return fail(error_out, "Resting order does not match its price level");
            }
            if (!first_order && order.sequence <= previous_sequence) {
                return fail(error_out, "FIFO queue sequence numbers are not strictly increasing");
            }
            first_order = false;
            previous_sequence = order.sequence;
            if (std::numeric_limits<Quantity>::max() - summed_volume < order.remaining_quantity) {
                return fail(error_out, "Price level volume sum overflows Quantity");
            }
            summed_volume += order.remaining_quantity;

            const auto found = lookup.find(order.id);
            if (found == lookup.end()) {
                return fail(error_out, "Resting order is missing from the lookup index");
            }
            if (found->second.side != expected_side || found->second.price != price ||
                found->second.iterator != it) {
                return fail(error_out, "Lookup index does not point to the matching order");
            }
        }
        if (summed_volume != level.total_volume) {
            return fail(error_out, "Cached price level volume does not match queued orders");
        }
    }
    return true;
}

} // namespace

std::optional<Price> OrderBook::best_bid() const noexcept {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const noexcept {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::optional<Price> OrderBook::spread() const noexcept {
    if (bids_.empty() || asks_.empty()) return std::nullopt;
    return asks_.begin()->first - bids_.begin()->first;
}

bool OrderBook::has_order(const OrderId id) const noexcept {
    return order_lookup_.contains(id);
}

const Order* OrderBook::get_order(const OrderId id) const noexcept {
    const auto found = order_lookup_.find(id);
    if (found == order_lookup_.end()) return nullptr;
    return &*found->second.iterator;
}

std::size_t OrderBook::active_order_count() const noexcept {
    return order_lookup_.size();
}

bool OrderBook::insert_resting_order(const Order& order) {
    if (order.id == 0 || order.type != OrderType::Limit || order.price <= 0 ||
        order.remaining_quantity == 0 || order.remaining_quantity > order.initial_quantity ||
        order_lookup_.contains(order.id)) {
        return false;
    }
    if ((order.side == Side::Buy && !asks_.empty() && order.price >= asks_.begin()->first) ||
        (order.side == Side::Sell && !bids_.empty() && order.price <= bids_.begin()->first)) {
        return false;
    }

    if (order.side == Side::Buy) {
        auto [level_it, inserted] = bids_.try_emplace(order.price);
        PriceLevel& level = level_it->second;
        if (inserted) level.price = order.price;
        if (std::numeric_limits<Quantity>::max() - level.total_volume < order.remaining_quantity) {
            if (inserted) bids_.erase(level_it);
            return false;
        }
        level.orders.push_back(order);
        auto order_it = std::prev(level.orders.end());
        level.total_volume += order.remaining_quantity;
        try {
            order_lookup_.emplace(order.id, OrderLocation{order.side, order.price, order_it});
        } catch (...) {
            level.total_volume -= order.remaining_quantity;
            level.orders.erase(order_it);
            if (level.orders.empty()) bids_.erase(level_it);
            throw;
        }
    } else {
        auto [level_it, inserted] = asks_.try_emplace(order.price);
        PriceLevel& level = level_it->second;
        if (inserted) level.price = order.price;
        if (std::numeric_limits<Quantity>::max() - level.total_volume < order.remaining_quantity) {
            if (inserted) asks_.erase(level_it);
            return false;
        }
        level.orders.push_back(order);
        auto order_it = std::prev(level.orders.end());
        level.total_volume += order.remaining_quantity;
        try {
            order_lookup_.emplace(order.id, OrderLocation{order.side, order.price, order_it});
        } catch (...) {
            level.total_volume -= order.remaining_quantity;
            level.orders.erase(order_it);
            if (level.orders.empty()) asks_.erase(level_it);
            throw;
        }
    }
    return true;
}

CancelResult OrderBook::cancel_order(const OrderId id) {
    CancelResult result{};
    result.order_id = id;
    const auto found = order_lookup_.find(id);
    if (found == order_lookup_.end()) {
        result.message = "Order ID not found";
        return result;
    }

    const OrderLocation location = found->second;
    const Quantity remaining = location.iterator->remaining_quantity;
    if (location.side == Side::Buy) {
        auto level_it = bids_.find(location.price);
        if (level_it == bids_.end()) {
            result.message = "Order book lookup invariant violated";
            return result;
        }
        level_it->second.total_volume -= remaining;
        level_it->second.orders.erase(location.iterator);
        if (level_it->second.orders.empty()) bids_.erase(level_it);
    } else {
        auto level_it = asks_.find(location.price);
        if (level_it == asks_.end()) {
            result.message = "Order book lookup invariant violated";
            return result;
        }
        level_it->second.total_volume -= remaining;
        level_it->second.orders.erase(location.iterator);
        if (level_it->second.orders.empty()) asks_.erase(level_it);
    }
    order_lookup_.erase(found);
    result.success = true;
    result.status = OrderStatus::Cancelled;
    result.cancelled_price = location.price;
    result.cancelled_quantity = remaining;
    result.message = "Cancelled " + std::to_string(remaining) + " units @ " +
                     std::to_string(location.price);
    return result;
}

void OrderBook::match_against_book(Order& incoming, SequenceNumber& seq_counter,
                                   TradeId& trade_id_counter, const TimestampNs now,
                                   std::vector<Trade>& out_trades) {
    while (incoming.remaining_quantity > 0) {
        const bool buying = incoming.side == Side::Buy;
        if ((buying && asks_.empty()) || (!buying && bids_.empty())) break;

        PriceLevel* level = nullptr;
        Price best_price = 0;
        if (buying) {
            auto level_it = asks_.begin();
            best_price = level_it->first;
            if (incoming.type == OrderType::Limit && incoming.price < best_price) break;
            level = &level_it->second;
        } else {
            auto level_it = bids_.begin();
            best_price = level_it->first;
            if (incoming.type == OrderType::Limit && incoming.price > best_price) break;
            level = &level_it->second;
        }

        while (incoming.remaining_quantity > 0 && !level->orders.empty()) {
            Order& resting = level->orders.front();
            const Quantity fill_quantity = std::min(incoming.remaining_quantity,
                                                   resting.remaining_quantity);
            incoming.remaining_quantity -= fill_quantity;
            resting.remaining_quantity -= fill_quantity;
            level->total_volume -= fill_quantity;

            Trade trade{};
            trade.trade_id = ++trade_id_counter;
            trade.buy_order_id = buying ? incoming.id : resting.id;
            trade.sell_order_id = buying ? resting.id : incoming.id;
            trade.aggressor_order_id = incoming.id;
            trade.resting_order_id = resting.id;
            trade.aggressor_side = incoming.side;
            trade.price = resting.price;
            trade.quantity = fill_quantity;
            trade.sequence = ++seq_counter;
            trade.timestamp_ns = now;
            out_trades.push_back(trade);

            if (resting.remaining_quantity == 0) {
                order_lookup_.erase(resting.id);
                level->orders.pop_front();
            }
        }

        if (level->orders.empty()) {
            if (buying) asks_.erase(asks_.begin());
            else bids_.erase(bids_.begin());
        }
    }
}

template <typename Levels>
void OrderBook::append_level_snapshot(const Levels& levels, const std::size_t depth,
                                      std::vector<PriceLevelSummary>& out) const {
    out.reserve(std::min(depth, levels.size()));
    for (const auto& [price, level] : levels) {
        if (out.size() == depth) break;
        out.push_back(PriceLevelSummary{price, level.total_volume, level.orders.size()});
    }
}

BookSnapshot OrderBook::get_snapshot(const std::size_t depth) const {
    BookSnapshot snapshot{};
    append_level_snapshot(bids_, depth, snapshot.bids);
    append_level_snapshot(asks_, depth, snapshot.asks);

    if (!bids_.empty()) {
        snapshot.has_best_bid = true;
        snapshot.best_bid = bids_.begin()->first;
        snapshot.best_bid_volume = bids_.begin()->second.total_volume;
    }
    if (!asks_.empty()) {
        snapshot.has_best_ask = true;
        snapshot.best_ask = asks_.begin()->first;
        snapshot.best_ask_volume = asks_.begin()->second.total_volume;
    }
    if (snapshot.has_best_bid && snapshot.has_best_ask) {
        snapshot.has_spread = true;
        snapshot.spread = snapshot.best_ask - snapshot.best_bid;
    }

    snapshot.resting_orders.reserve(order_lookup_.size());
    for (const auto& [price, level] : bids_) {
        (void)price;
        snapshot.resting_orders.insert(snapshot.resting_orders.end(), level.orders.begin(), level.orders.end());
    }
    for (const auto& [price, level] : asks_) {
        (void)price;
        snapshot.resting_orders.insert(snapshot.resting_orders.end(), level.orders.begin(), level.orders.end());
    }
    return snapshot;
}

void OrderBook::clear() noexcept {
    bids_.clear();
    asks_.clear();
    order_lookup_.clear();
}

bool OrderBook::verify_invariants(std::string* error_out) const {
    if (error_out != nullptr) error_out->clear();
    if (!check_price_map(bids_, Side::Buy, order_lookup_, error_out) ||
        !check_price_map(asks_, Side::Sell, order_lookup_, error_out)) {
        return false;
    }
    if (!bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first) {
        return fail(error_out, "Resting book is crossed");
    }

    const std::size_t total_orders = count_orders(bids_) + count_orders(asks_);
    if (order_lookup_.size() != total_orders) {
        return fail(error_out, "Lookup index size differs from resting order count");
    }
    for (const auto& [id, location] : order_lookup_) {
        if (location.side == Side::Buy) {
            const auto level_it = bids_.find(location.price);
            if (level_it == bids_.end()) return fail(error_out, "Lookup index references missing bid level");
            if (location.iterator == level_it->second.orders.end() || location.iterator->id != id ||
                location.iterator->side != Side::Buy || location.iterator->price != location.price) {
                return fail(error_out, "Lookup index has an invalid bid iterator");
            }
        } else {
            const auto level_it = asks_.find(location.price);
            if (level_it == asks_.end()) return fail(error_out, "Lookup index references missing ask level");
            if (location.iterator == level_it->second.orders.end() || location.iterator->id != id ||
                location.iterator->side != Side::Sell || location.iterator->price != location.price) {
                return fail(error_out, "Lookup index has an invalid ask iterator");
            }
        }
    }
    return true;
}

} // namespace atlas
