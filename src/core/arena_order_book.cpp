#include "basalt/core/arena_order_book.hpp"

#include <algorithm>
#include <limits>

namespace basalt {
namespace {

bool fail(std::string* error_out, const std::string& message) {
    if (error_out) *error_out = message;
    return false;
}

} // namespace

ArenaOrderBook::ArenaOrderBook(const std::size_t order_capacity)
    : pool_(order_capacity), links_(order_capacity) {}

bool ArenaOrderBook::has_order(const OrderId id) const noexcept { return locations_.contains(id); }

const Order* ArenaOrderBook::get_order(const OrderId id) const noexcept {
    const auto found = locations_.find(id);
    return found == locations_.end() ? nullptr : pool_.get(found->second);
}

std::optional<Price> ArenaOrderBook::best_bid() const noexcept {
    return bids_.empty() ? std::nullopt : std::optional<Price>{bids_.begin()->first};
}

std::optional<Price> ArenaOrderBook::best_ask() const noexcept {
    return asks_.empty() ? std::nullopt : std::optional<Price>{asks_.begin()->first};
}

ArenaOrderBook::Index ArenaOrderBook::index_for(const OrderId id) const noexcept {
    const auto found = locations_.find(id);
    return found == locations_.end() ? no_index : found->second.index;
}

bool ArenaOrderBook::link_order(const Order& order) {
    last_error_.clear();
    if (order.id == 0 || order.type != OrderType::Limit || order.price <= 0 ||
        (order.side != Side::Buy && order.side != Side::Sell) || order.remaining_quantity == 0 ||
        order.remaining_quantity > order.initial_quantity || has_order(order.id)) {
        last_error_ = "Invalid or duplicate resting order";
        return false;
    }
    if ((order.side == Side::Buy && best_ask() && order.price >= *best_ask()) ||
        (order.side == Side::Sell && best_bid() && order.price <= *best_bid())) {
        last_error_ = "Resting order would cross the opposite book";
        return false;
    }
    if (order.side == Side::Buy) {
        const auto level = bids_.find(order.price);
        if (level != bids_.end() && std::numeric_limits<Quantity>::max() - level->second.total_volume < order.remaining_quantity) {
            last_error_ = "Price level volume overflow";
            return false;
        }
    } else {
        const auto level = asks_.find(order.price);
        if (level != asks_.end() && std::numeric_limits<Quantity>::max() - level->second.total_volume < order.remaining_quantity) {
            last_error_ = "Price level volume overflow";
            return false;
        }
    }
    auto acquired = pool_.acquire(order);
    if (!acquired) {
        last_error_ = acquired.error().code;
        return false;
    }
    const PoolHandle handle = acquired.value();
    const Index index = handle.index;
    links_[index] = Link{handle, no_index, no_index};
    if (order.side == Side::Buy) {
        auto [level, inserted] = bids_.try_emplace(order.price);
        (void)inserted;
        links_[index].previous = level->second.tail;
        if (level->second.tail == no_index) level->second.head = index;
        else links_[level->second.tail].next = index;
        level->second.tail = index;
        level->second.total_volume += order.remaining_quantity;
    } else {
        auto [level, inserted] = asks_.try_emplace(order.price);
        (void)inserted;
        links_[index].previous = level->second.tail;
        if (level->second.tail == no_index) level->second.head = index;
        else links_[level->second.tail].next = index;
        level->second.tail = index;
        level->second.total_volume += order.remaining_quantity;
    }
    locations_.emplace(order.id, handle);
    return true;
}

bool ArenaOrderBook::insert_resting_order(const Order& order) { return link_order(order); }

void ArenaOrderBook::unlink_order(const Index index, Level& level) noexcept {
    const Link link = links_[index];
    const Order* order = pool_.get(link.handle);
    if (link.previous == no_index) level.head = link.next;
    else links_[link.previous].next = link.next;
    if (link.next == no_index) level.tail = link.previous;
    else links_[link.next].previous = link.previous;
    if (order) locations_.erase(order->id);
    (void)pool_.release(link.handle);
    links_[index] = Link{};
}

CancelResult ArenaOrderBook::cancel_order(const OrderId id) {
    CancelResult result{};
    result.order_id = id;
    const auto found = locations_.find(id);
    if (found == locations_.end()) { result.message = "Order ID not found"; return result; }
    const auto handle = found->second;
    const auto* order = pool_.get(handle);
    if (!order) { result.message = "Pool handle generation mismatch"; return result; }
    result.cancelled_price = order->price;
    result.cancelled_quantity = order->remaining_quantity;
    if (order->side == Side::Buy) {
        auto level = bids_.find(order->price);
        if (level == bids_.end()) { result.message = "Missing bid price level"; return result; }
        level->second.total_volume -= order->remaining_quantity;
        unlink_order(handle.index, level->second);
        if (level->second.head == no_index) bids_.erase(level);
    } else {
        auto level = asks_.find(order->price);
        if (level == asks_.end()) { result.message = "Missing ask price level"; return result; }
        level->second.total_volume -= order->remaining_quantity;
        unlink_order(handle.index, level->second);
        if (level->second.head == no_index) asks_.erase(level);
    }
    result.success = true;
    result.status = OrderStatus::Cancelled;
    result.message = "Cancelled pooled order";
    return result;
}

void ArenaOrderBook::match_against_book(Order& incoming, SequenceNumber& sequence_counter,
                                       TradeId& trade_id_counter, const TimestampNs now,
                                       std::vector<Trade>& out_trades) {
    last_error_.clear();
    while (incoming.remaining_quantity > 0) {
        const bool buying = incoming.side == Side::Buy;
        if ((buying && asks_.empty()) || (!buying && bids_.empty())) break;
        if (buying) {
            auto level = asks_.begin();
            if (incoming.type == OrderType::Limit && incoming.price < level->first) break;
            Index index = level->second.head;
            while (incoming.remaining_quantity > 0 && index != no_index) {
                const auto handle = links_[index].handle;
                auto* resting = pool_.get(handle);
                if (!resting) break;
                const Quantity fill = std::min(incoming.remaining_quantity, resting->remaining_quantity);
                incoming.remaining_quantity -= fill;
                resting->remaining_quantity -= fill;
                level->second.total_volume -= fill;
                Trade trade{};
                trade.trade_id = ++trade_id_counter;
                trade.buy_order_id = incoming.id;
                trade.sell_order_id = resting->id;
                trade.aggressor_order_id = incoming.id;
                trade.resting_order_id = resting->id;
                trade.aggressor_side = Side::Buy;
                trade.price = resting->price;
                trade.quantity = fill;
                trade.sequence = ++sequence_counter;
                trade.timestamp_ns = now;
                out_trades.push_back(trade);
                const Index next = links_[index].next;
                if (resting->remaining_quantity == 0) unlink_order(index, level->second);
                index = next;
            }
            if (level->second.head == no_index) asks_.erase(level);
        } else {
            auto level = bids_.begin();
            if (incoming.type == OrderType::Limit && incoming.price > level->first) break;
            Index index = level->second.head;
            while (incoming.remaining_quantity > 0 && index != no_index) {
                const auto handle = links_[index].handle;
                auto* resting = pool_.get(handle);
                if (!resting) break;
                const Quantity fill = std::min(incoming.remaining_quantity, resting->remaining_quantity);
                incoming.remaining_quantity -= fill;
                resting->remaining_quantity -= fill;
                level->second.total_volume -= fill;
                Trade trade{};
                trade.trade_id = ++trade_id_counter;
                trade.buy_order_id = resting->id;
                trade.sell_order_id = incoming.id;
                trade.aggressor_order_id = incoming.id;
                trade.resting_order_id = resting->id;
                trade.aggressor_side = Side::Sell;
                trade.price = resting->price;
                trade.quantity = fill;
                trade.sequence = ++sequence_counter;
                trade.timestamp_ns = now;
                out_trades.push_back(trade);
                const Index next = links_[index].next;
                if (resting->remaining_quantity == 0) unlink_order(index, level->second);
                index = next;
            }
            if (level->second.head == no_index) bids_.erase(level);
        }
    }
    if (incoming.remaining_quantity > 0 && incoming.type == OrderType::Limit)
        (void)link_order(incoming);
}

BookSnapshot ArenaOrderBook::get_snapshot(const std::size_t depth) const {
    BookSnapshot snapshot{};
    const auto append = [&](const auto& levels, auto& summaries) {
        for (const auto& [price, level] : levels) {
            if (summaries.size() == depth) break;
            std::size_t count = 0;
            for (Index index = level.head; index != no_index; index = links_[index].next) ++count;
            summaries.push_back({price, level.total_volume, count});
        }
    };
    append(bids_, snapshot.bids);
    append(asks_, snapshot.asks);
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
    const auto append_orders = [&](const auto& levels) {
        for (const auto& [price, level] : levels) {
            (void)price;
            for (Index index = level.head; index != no_index; index = links_[index].next) {
                if (const auto* order = pool_.get(links_[index].handle)) snapshot.resting_orders.push_back(*order);
            }
        }
    };
    append_orders(bids_);
    append_orders(asks_);
    return snapshot;
}

bool ArenaOrderBook::verify_invariants(std::string* error_out) const {
    if (error_out) error_out->clear();
    std::size_t count = 0;
    const auto check = [&](const auto& levels, const Side side) {
        for (const auto& [price, level] : levels) {
            if (price <= 0 || level.head == no_index || level.tail == no_index || level.total_volume == 0)
                return fail(error_out, "Arena price level is empty or invalid");
            Quantity volume = 0;
            SequenceNumber previous_sequence = 0;
            Index previous = no_index;
            for (Index index = level.head; index != no_index; index = links_[index].next) {
                if (index >= links_.size()) return fail(error_out, "Arena link index is out of bounds");
                const auto& link = links_[index];
                const auto* order = pool_.get(link.handle);
                if (!order || link.previous != previous || order->id == 0 || order->side != side ||
                    order->type != OrderType::Limit || order->price != price || order->remaining_quantity == 0 ||
                    order->remaining_quantity > order->initial_quantity || !locations_.contains(order->id) ||
                    locations_.at(order->id) != link.handle || (previous != no_index && order->sequence <= previous_sequence))
                    return fail(error_out, "Arena order node, generation, priority or lookup invariant failed");
                if (std::numeric_limits<Quantity>::max() - volume < order->remaining_quantity)
                    return fail(error_out, "Arena level volume sum overflows");
                volume += order->remaining_quantity;
                previous_sequence = order->sequence;
                previous = index;
                ++count;
            }
            if (previous != level.tail || volume != level.total_volume)
                return fail(error_out, "Arena tail or cached level volume mismatch");
        }
        return true;
    };
    if (!check(bids_, Side::Buy) || !check(asks_, Side::Sell)) return false;
    if (!bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first)
        return fail(error_out, "Arena book is crossed");
    if (count != locations_.size() || count != pool_.active_count())
        return fail(error_out, "Arena active order count mismatch");
    return true;
}

void ArenaOrderBook::clear() noexcept {
    for (const auto& [id, handle] : locations_) {
        (void)id;
        (void)pool_.release(handle);
    }
    locations_.clear();
    bids_.clear();
    asks_.clear();
    last_error_.clear();
}

} // namespace basalt
