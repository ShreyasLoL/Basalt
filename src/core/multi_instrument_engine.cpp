#include "basalt/core/multi_instrument_engine.hpp"

#include <algorithm>
#include <limits>
#include <tuple>

namespace basalt {
namespace {

bool valid_side(const Side side) noexcept { return side == Side::Buy || side == Side::Sell; }

bool valid_type(const AdvancedOrderType type) noexcept {
    switch (type) {
    case AdvancedOrderType::Limit:
    case AdvancedOrderType::Market:
    case AdvancedOrderType::Ioc:
    case AdvancedOrderType::Fok:
    case AdvancedOrderType::PostOnly:
    case AdvancedOrderType::StopMarket:
    case AdvancedOrderType::StopLimit: return true;
    }
    return false;
}

bool same_owner(const AdvancedOrderRequest& left, const AdvancedOrderRequest& right) noexcept {
    const bool same_account = !left.account_id.empty() && left.account_id == right.account_id;
    const bool same_participant = !left.participant_id.empty() && left.participant_id == right.participant_id;
    return same_account || same_participant;
}

} // namespace

std::string_view to_string(const AdvancedOrderType type) noexcept {
    switch (type) {
    case AdvancedOrderType::Limit: return "LIMIT";
    case AdvancedOrderType::Market: return "MARKET";
    case AdvancedOrderType::Ioc: return "IOC";
    case AdvancedOrderType::Fok: return "FOK";
    case AdvancedOrderType::PostOnly: return "POST_ONLY";
    case AdvancedOrderType::StopMarket: return "STOP_MARKET";
    case AdvancedOrderType::StopLimit: return "STOP_LIMIT";
    }
    return "UNKNOWN";
}

std::string_view to_string(const STPPolicy policy) noexcept {
    switch (policy) {
    case STPPolicy::CancelNewest: return "CANCEL_NEWEST";
    case STPPolicy::CancelOldest: return "CANCEL_OLDEST";
    case STPPolicy::CancelBoth: return "CANCEL_BOTH";
    }
    return "UNKNOWN";
}

bool MultiInstrumentEngine::would_cross(const InstrumentBook& book, const Side side,
                                         const Price price) const noexcept {
    if (side == Side::Buy) return !book.asks.empty() && price >= book.asks.begin()->first;
    return !book.bids.empty() && price <= book.bids.begin()->first;
}

bool MultiInstrumentEngine::can_fill(const InstrumentBook& book,
                                      const AdvancedOrderRequest& request) const noexcept {
    Quantity available = 0;
    const auto consider = [&](const auto& levels) {
        for (const auto& [price, orders] : levels) {
            if (request.side == Side::Buy ? price > request.price : price < request.price) break;
            for (const auto& resting : orders) {
                if (same_owner(request, resting.request)) {
                    if (request.stp_policy == STPPolicy::CancelNewest ||
                        request.stp_policy == STPPolicy::CancelBoth) return false;
                    continue;
                }
                if (std::numeric_limits<Quantity>::max() - available < resting.remaining_quantity)
                    return true;
                available += resting.remaining_quantity;
                if (available >= request.quantity) return true;
            }
        }
        return false;
    };
    if (request.side == Side::Buy) return consider(book.asks);
    return consider(book.bids);
}

bool MultiInstrumentEngine::rest(InstrumentBook& book, const AdvancedOrderRequest& request,
                                  const SequenceNumber sequence, const Quantity remaining_quantity) {
    if (request.id == 0 || request.price <= 0 || request.quantity == 0 || remaining_quantity == 0 ||
        remaining_quantity > request.quantity ||
        locations_.contains(request.id) || would_cross(book, request.side, request.price)) return false;
    if (request.side == Side::Buy) {
        auto [level, inserted] = book.bids.try_emplace(request.price);
        (void)inserted;
        level->second.push_back(RestingOrder{request, remaining_quantity, sequence});
        auto iterator = std::prev(level->second.end());
        locations_.emplace(request.id, OrderLocation{request.instrument_id, request.side,
                                                     request.price, iterator});
    } else {
        auto [level, inserted] = book.asks.try_emplace(request.price);
        (void)inserted;
        level->second.push_back(RestingOrder{request, remaining_quantity, sequence});
        auto iterator = std::prev(level->second.end());
        locations_.emplace(request.id, OrderLocation{request.instrument_id, request.side,
                                                     request.price, iterator});
    }
    return true;
}

void MultiInstrumentEngine::erase_resting(const OrderId id) noexcept {
    const auto found = locations_.find(id);
    if (found == locations_.end()) return;
    const auto location = found->second;
    const auto book_it = books_.find(location.instrument_id);
    if (book_it == books_.end()) return;
    if (location.side == Side::Buy) {
        const auto level = book_it->second.bids.find(location.price);
        if (level != book_it->second.bids.end()) {
            level->second.erase(location.iterator);
            if (level->second.empty()) book_it->second.bids.erase(level);
        }
    } else {
        const auto level = book_it->second.asks.find(location.price);
        if (level != book_it->second.asks.end()) {
            level->second.erase(location.iterator);
            if (level->second.empty()) book_it->second.asks.erase(level);
        }
    }
    locations_.erase(found);
}

AdvancedOrderResult MultiInstrumentEngine::submit_order(const AdvancedOrderRequest& request,
                                                         const TimestampNs timestamp_ns) {
    AdvancedOrderResult result{};
    result.instrument_id = request.instrument_id;
    result.order_id = request.id;
    result.initial_quantity = request.quantity;
    result.remaining_quantity = request.quantity;
    const bool stop = request.type == AdvancedOrderType::StopMarket || request.type == AdvancedOrderType::StopLimit;
    const bool priced = request.type == AdvancedOrderType::Limit || request.type == AdvancedOrderType::Ioc ||
                        request.type == AdvancedOrderType::Fok || request.type == AdvancedOrderType::PostOnly ||
                        request.type == AdvancedOrderType::StopLimit;
    std::string invalid;
    if (request.id == 0 || request.instrument_id == 0) invalid = "Order and instrument IDs must be positive";
    else if (locations_.contains(request.id) || stops_.contains(request.id)) invalid = "Duplicate active order ID";
    else if (!valid_side(request.side) || !valid_type(request.type)) invalid = "Invalid order side or type";
    else if (request.quantity == 0 || request.quantity > MAX_ORDER_QUANTITY) invalid = "Invalid order quantity";
    else if (priced && (request.price <= 0 || request.price > MAX_ORDER_PRICE)) invalid = "Invalid limit price";
    else if (stop && (request.stop_price <= 0 || request.stop_price > MAX_ORDER_PRICE)) invalid = "Invalid stop trigger";
    else if (!stop && request.stop_price != 0) invalid = "Stop trigger is only valid for stop orders";
    else if (request.type == AdvancedOrderType::StopMarket && request.price != 0) invalid = "Stop-market price must be zero";
    else if (request.type == AdvancedOrderType::Market && request.price != 0) invalid = "Market order price must be zero";
    if (!invalid.empty()) {
        result.status = OrderStatus::Rejected;
        result.sequence = ++next_sequence_;
        result.message = std::move(invalid);
        return result;
    }

    const auto existing_book = books_.find(request.instrument_id);
    const InstrumentBook empty_book{};
    const auto& read_book = existing_book == books_.end() ? empty_book : existing_book->second;
    // A failed FOK is a pure pre-check: it changes no book, sequence or trade ID.
    if (request.type == AdvancedOrderType::Fok && !can_fill(read_book, request)) {
        result.status = OrderStatus::Cancelled;
        result.sequence = 0;
        result.message = "FOK cancelled: displayed eligible liquidity is insufficient";
        return result;
    }
    const auto sequence = ++next_sequence_;
    result.sequence = sequence;
    if (stop) {
        stops_.emplace(request.id, StopOrder{request, sequence});
        result.status = OrderStatus::New;
        result.message = "Stop order accepted and awaiting a reference trade trigger";
        return result;
    }
    (void)books_[request.instrument_id];
    return submit_validated(request, timestamp_ns);
}

AdvancedOrderResult MultiInstrumentEngine::submit_validated(const AdvancedOrderRequest& request,
                                                              TimestampNs timestamp_ns) {
    auto& book = books_[request.instrument_id];
    AdvancedOrderResult result{};
    result.instrument_id = request.instrument_id;
    result.order_id = request.id;
    result.initial_quantity = request.quantity;
    result.remaining_quantity = request.quantity;
    result.sequence = next_sequence_;
    if (request.type == AdvancedOrderType::PostOnly && would_cross(book, request.side, request.price)) {
        result.status = OrderStatus::Rejected;
        result.message = "Post-only order would immediately take liquidity";
        return result;
    }
    if (timestamp_ns == 0) timestamp_ns = logical_time_ + 1;
    if (timestamp_ns <= logical_time_) timestamp_ns = logical_time_ + 1;
    logical_time_ = timestamp_ns;
    const bool marketable = request.type == AdvancedOrderType::Market || request.type == AdvancedOrderType::Ioc ||
                            request.type == AdvancedOrderType::Fok || request.type == AdvancedOrderType::Limit;
    Quantity remaining = request.quantity;
    bool incoming_cancelled = false;
    while (remaining > 0 && !incoming_cancelled) {
        if (request.side == Side::Buy) {
            if (book.asks.empty()) break;
            auto level = book.asks.begin();
            if (request.type != AdvancedOrderType::Market && request.price < level->first) break;
            auto order = level->second.begin();
            while (remaining > 0 && order != level->second.end()) {
                const auto resting_copy = *order;
                if (same_owner(request, resting_copy.request)) {
                    if (request.stp_policy == STPPolicy::CancelNewest) {
                        incoming_cancelled = true;
                        result.message = "Self-trade prevention cancelled the incoming order";
                        break;
                    }
                    locations_.erase(order->request.id);
                    order = level->second.erase(order);
                    if (request.stp_policy == STPPolicy::CancelBoth) {
                        incoming_cancelled = true;
                        result.message = "Self-trade prevention cancelled both orders";
                        break;
                    }
                    continue;
                }
                const auto fill = std::min(remaining, order->remaining_quantity);
                remaining -= fill;
                order->remaining_quantity -= fill;
                const auto& resting = resting_copy.request;
                Trade trade{};
                trade.trade_id = ++next_trade_id_;
                trade.buy_order_id = request.id;
                trade.sell_order_id = resting.id;
                trade.aggressor_order_id = request.id;
                trade.resting_order_id = resting.id;
                trade.aggressor_side = request.side;
                trade.price = resting_copy.request.price;
                trade.quantity = fill;
                trade.sequence = ++next_sequence_;
                trade.timestamp_ns = timestamp_ns;
                result.trades.push_back({request.instrument_id, trade, request.account_id, resting.account_id,
                                         request.participant_id, resting.participant_id});
                if (order->remaining_quantity == 0) {
                    locations_.erase(resting.id);
                    order = level->second.erase(order);
                } else ++order;
            }
            if (level->second.empty()) book.asks.erase(level);
        } else {
            if (book.bids.empty()) break;
            auto level = book.bids.begin();
            if (request.type != AdvancedOrderType::Market && request.price > level->first) break;
            auto order = level->second.begin();
            while (remaining > 0 && order != level->second.end()) {
                const auto resting_copy = *order;
                if (same_owner(request, resting_copy.request)) {
                    if (request.stp_policy == STPPolicy::CancelNewest) {
                        incoming_cancelled = true;
                        result.message = "Self-trade prevention cancelled the incoming order";
                        break;
                    }
                    locations_.erase(order->request.id);
                    order = level->second.erase(order);
                    if (request.stp_policy == STPPolicy::CancelBoth) {
                        incoming_cancelled = true;
                        result.message = "Self-trade prevention cancelled both orders";
                        break;
                    }
                    continue;
                }
                const auto fill = std::min(remaining, order->remaining_quantity);
                remaining -= fill;
                order->remaining_quantity -= fill;
                const auto& resting = resting_copy.request;
                Trade trade{};
                trade.trade_id = ++next_trade_id_;
                trade.buy_order_id = resting.id;
                trade.sell_order_id = request.id;
                trade.aggressor_order_id = request.id;
                trade.resting_order_id = resting.id;
                trade.aggressor_side = request.side;
                trade.price = resting_copy.request.price;
                trade.quantity = fill;
                trade.sequence = ++next_sequence_;
                trade.timestamp_ns = timestamp_ns;
                result.trades.push_back({request.instrument_id, trade, resting.account_id, request.account_id,
                                         resting.participant_id, request.participant_id});
                if (order->remaining_quantity == 0) {
                    locations_.erase(resting.id);
                    order = level->second.erase(order);
                } else ++order;
            }
            if (level->second.empty()) book.bids.erase(level);
        }
        if (!marketable) break;
    }
    result.filled_quantity = request.quantity - remaining;
    result.remaining_quantity = remaining;
    const bool immediate_only = request.type == AdvancedOrderType::Market || request.type == AdvancedOrderType::Ioc ||
                                request.type == AdvancedOrderType::Fok;
    if (remaining == 0) result.status = OrderStatus::Filled;
    else if (!incoming_cancelled && !immediate_only && request.type != AdvancedOrderType::PostOnly) {
        auto resting_request = request;
        resting_request.type = AdvancedOrderType::Limit;
        if (rest(book, resting_request, result.sequence, remaining)) result.status = result.filled_quantity > 0
            ? OrderStatus::PartiallyFilled : OrderStatus::New;
        else result.status = OrderStatus::Rejected;
    } else {
        result.status = result.filled_quantity > 0 ? OrderStatus::PartiallyFilled : OrderStatus::Cancelled;
    }
    if (result.message.empty()) result.message = result.status == OrderStatus::Rejected
        ? "Could not rest order remainder" : "Order processed";
    return result;
}

CancelResult MultiInstrumentEngine::cancel_order(const common::InstrumentId instrument_id,
                                                 const OrderId order_id) {
    CancelResult result{};
    result.order_id = order_id;
    const auto located = locations_.find(order_id);
    if (located != locations_.end() && located->second.instrument_id == instrument_id) {
        result.success = true;
        result.cancelled_price = located->second.price;
        result.cancelled_quantity = located->second.iterator->remaining_quantity;
        erase_resting(order_id);
    } else {
        const auto stop = stops_.find(order_id);
        if (stop == stops_.end() || stop->second.request.instrument_id != instrument_id) {
            result.message = "Active order not found for instrument";
            return result;
        }
        result.success = true;
        result.cancelled_price = stop->second.request.price;
        result.cancelled_quantity = stop->second.request.quantity;
        stops_.erase(stop);
    }
    result.status = OrderStatus::Cancelled;
    result.sequence = ++next_sequence_;
    result.message = "Cancelled active order";
    return result;
}

ModifyOrderResult MultiInstrumentEngine::modify_order(const ModifyOrderRequest& request) {
    ModifyOrderResult result{};
    const auto found = locations_.find(request.order_id);
    if (found == locations_.end() || found->second.instrument_id != request.instrument_id) {
        result.message = "Only a resting order on the selected instrument can be modified";
        return result;
    }
    auto& location = found->second;
    auto& order = *location.iterator;
    const auto filled = order.request.quantity - order.remaining_quantity;
    if (request.new_total_quantity <= filled || request.new_total_quantity > MAX_ORDER_QUANTITY ||
        request.new_price <= 0 || request.new_price > MAX_ORDER_PRICE) {
        result.message = "Modified price or total quantity is invalid";
        return result;
    }
    auto& book = books_.at(request.instrument_id);
    if (would_cross(book, location.side, request.new_price)) {
        result.message = "Modification would cross the opposite book";
        return result;
    }
    const auto new_remaining = request.new_total_quantity - filled;
    const bool resets = request.new_price != order.request.price ||
                        request.new_total_quantity > order.request.quantity;
    if (!resets) {
        order.request.quantity = request.new_total_quantity;
        order.remaining_quantity = new_remaining;
        result.success = true;
        result.priority_reset = false;
        result.sequence = order.sequence;
        result.remaining_quantity = new_remaining;
        result.message = "Quantity decrease retained time priority";
        return result;
    }
    auto modified = order;
    const auto side = location.side;
    erase_resting(request.order_id);
    modified.request.price = request.new_price;
    modified.request.quantity = request.new_total_quantity;
    modified.remaining_quantity = new_remaining;
    modified.sequence = ++next_sequence_;
    const auto insert_modified = [&](auto& target_levels) {
        auto [level, inserted] = target_levels.try_emplace(request.new_price);
        (void)inserted;
        level->second.push_back(modified);
        auto iterator = std::prev(level->second.end());
        locations_.emplace(request.order_id,
            OrderLocation{request.instrument_id, side, request.new_price, iterator});
    };
    if (side == Side::Buy) insert_modified(book.bids);
    else insert_modified(book.asks);
    result.success = true;
    result.priority_reset = true;
    result.sequence = modified.sequence;
    result.remaining_quantity = new_remaining;
    result.message = "Price or quantity increase re-queued at the price level tail";
    return result;
}

common::Result<std::vector<AdvancedOrderResult>> MultiInstrumentEngine::process_reference_trade(
    const common::InstrumentId instrument_id, const Price reference_price, const TimestampNs timestamp_ns) {
    if (instrument_id == 0 || reference_price <= 0 || timestamp_ns == 0)
        return common::Result<std::vector<AdvancedOrderResult>>::failure(
            {"REFERENCE_TRADE_INVALID", "Stop triggers require an instrument, positive price and timestamp"});
    std::vector<std::pair<SequenceNumber, OrderId>> triggered;
    for (const auto& [id, stop] : stops_) {
        const auto& request = stop.request;
        if (request.instrument_id == instrument_id &&
            ((request.side == Side::Buy && reference_price >= request.stop_price) ||
             (request.side == Side::Sell && reference_price <= request.stop_price)))
            triggered.emplace_back(stop.sequence, id);
    }
    std::sort(triggered.begin(), triggered.end());
    std::vector<AdvancedOrderResult> results;
    results.reserve(triggered.size());
    for (const auto& [sequence, id] : triggered) {
        (void)sequence;
        const auto found = stops_.find(id);
        if (found == stops_.end()) continue;
        auto request = found->second.request;
        stops_.erase(found);
        request.type = request.type == AdvancedOrderType::StopMarket
            ? AdvancedOrderType::Market : AdvancedOrderType::Limit;
        request.stop_price = 0;
        results.push_back(submit_order(request, timestamp_ns));
    }
    return common::Result<std::vector<AdvancedOrderResult>>::success(std::move(results));
}

BookSnapshot MultiInstrumentEngine::get_snapshot(const common::InstrumentId instrument_id,
                                                const std::size_t depth) const {
    BookSnapshot snapshot{};
    const auto found = books_.find(instrument_id);
    if (found == books_.end()) return snapshot;
    const auto append = [&](const auto& levels, auto& summaries) {
        for (const auto& [price, orders] : levels) {
            if (summaries.size() == depth) break;
            Quantity volume = 0;
            for (const auto& order : orders) volume += order.remaining_quantity;
            summaries.push_back({price, volume, orders.size()});
        }
    };
    append(found->second.bids, snapshot.bids);
    append(found->second.asks, snapshot.asks);
    if (!found->second.bids.empty()) {
        snapshot.has_best_bid = true;
        snapshot.best_bid = found->second.bids.begin()->first;
        for (const auto& order : found->second.bids.begin()->second)
            snapshot.best_bid_volume += order.remaining_quantity;
    }
    if (!found->second.asks.empty()) {
        snapshot.has_best_ask = true;
        snapshot.best_ask = found->second.asks.begin()->first;
        for (const auto& order : found->second.asks.begin()->second)
            snapshot.best_ask_volume += order.remaining_quantity;
    }
    if (snapshot.has_best_bid && snapshot.has_best_ask) {
        snapshot.has_spread = true;
        snapshot.spread = snapshot.best_ask - snapshot.best_bid;
    }
    for (const auto& [price, orders] : found->second.bids) {
        (void)price;
        for (const auto& order : orders)
            snapshot.resting_orders.push_back({order.request.id, order.request.side, OrderType::Limit,
                order.request.price, order.request.quantity, order.remaining_quantity,
                order.sequence, logical_time_});
    }
    for (const auto& [price, orders] : found->second.asks) {
        (void)price;
        for (const auto& order : orders)
            snapshot.resting_orders.push_back({order.request.id, order.request.side, OrderType::Limit,
                order.request.price, order.request.quantity, order.remaining_quantity,
                order.sequence, logical_time_});
    }
    return snapshot;
}

bool MultiInstrumentEngine::verify_invariants(std::string* error_out) const {
    if (error_out) error_out->clear();
    const auto fail = [error_out](const char* message) {
        if (error_out) *error_out = message;
        return false;
    };
    std::size_t resting_count = 0;
    for (const auto& [instrument_id, book] : books_) {
        if (instrument_id == 0) return fail("Book has a zero instrument ID");
        if (!book.bids.empty() && !book.asks.empty() && book.bids.begin()->first >= book.asks.begin()->first)
            return fail("Instrument book is crossed");
        const auto check = [&](const auto& levels, const Side side) {
            for (const auto& [price, orders] : levels) {
                if (price <= 0 || orders.empty()) return false;
                SequenceNumber previous = 0;
                for (const auto& order : orders) {
                    ++resting_count;
                    if (order.request.instrument_id != instrument_id || order.request.side != side ||
                        order.request.id == 0 || order.request.price != price ||
                        order.request.quantity < order.remaining_quantity || order.remaining_quantity == 0 ||
                        order.sequence <= previous || !locations_.contains(order.request.id)) return false;
                    const auto& location = locations_.at(order.request.id);
                    if (location.instrument_id != instrument_id || location.price != price ||
                        location.side != side || &*location.iterator != &order) return false;
                    previous = order.sequence;
                }
            }
            return true;
        };
        if (!check(book.bids, Side::Buy) || !check(book.asks, Side::Sell))
            return fail("Price level order, volume, or lookup invariant failed");
    }
    if (resting_count != locations_.size()) return fail("Resting lookup count mismatch");
    for (const auto& [id, stop] : stops_) {
        if (id == 0 || stop.request.id != id || stop.request.instrument_id == 0 || stop.sequence == 0 ||
            (stop.request.type != AdvancedOrderType::StopMarket &&
             stop.request.type != AdvancedOrderType::StopLimit) || locations_.contains(id))
            return fail("Pending stop registry invariant failed");
    }
    return true;
}

std::size_t MultiInstrumentEngine::active_order_count() const noexcept {
    return locations_.size() + stops_.size();
}

std::vector<common::InstrumentId> MultiInstrumentEngine::instruments() const {
    std::vector<common::InstrumentId> result;
    result.reserve(books_.size());
    for (const auto& [instrument_id, book] : books_) {
        (void)book;
        result.push_back(instrument_id);
    }
    return result;
}

void MultiInstrumentEngine::reset() noexcept {
    books_.clear();
    locations_.clear();
    stops_.clear();
    next_sequence_ = 0;
    next_trade_id_ = 0;
    logical_time_ = 0;
}

} // namespace basalt
