#include "basalt/core/matching_engine.hpp"

#include <algorithm>
#include <limits>
#include <sstream>

namespace basalt {
namespace {

bool valid_side(const Side side) noexcept {
    return side == Side::Buy || side == Side::Sell;
}

bool valid_type(const OrderType type) noexcept {
    return type == OrderType::Limit || type == OrderType::Market;
}

std::string trade_message(const Trade& trade) {
    std::ostringstream message;
    message << "Executed " << trade.quantity << " @ " << trade.price << " (Buy #"
            << trade.buy_order_id << " vs Sell #" << trade.sell_order_id << ')';
    return message.str();
}

} // namespace

void MatchingEngine::append_trade(const Trade& trade) {
    recent_trades_.push_back(trade);
    if (recent_trades_.size() > HISTORY_CAPACITY) recent_trades_.pop_front();
    ++stats_.total_trades_executed;
    if (std::numeric_limits<Quantity>::max() - stats_.total_volume_traded < trade.quantity) {
        stats_.total_volume_traded = std::numeric_limits<Quantity>::max();
    } else {
        stats_.total_volume_traded += trade.quantity;
    }
    stats_.has_last_trade_price = true;
    stats_.last_trade_price = trade.price;
    stats_.last_trade_quantity = trade.quantity;

}

void MatchingEngine::append_activity(ActivityRecord record) {
    recent_activity_.push_back(std::move(record));
    if (recent_activity_.size() > HISTORY_CAPACITY) recent_activity_.pop_front();
}

void MatchingEngine::refresh_book_stats() noexcept {
    stats_.active_order_count = book_.active_order_count();
    stats_.bid_level_count = book_.bid_level_count();
    stats_.ask_level_count = book_.ask_level_count();
}

OrderResult MatchingEngine::submit_order(const OrderRequest& request) {
    OrderResult result{};
    result.order_id = request.id;
    result.initial_quantity = request.quantity;
    result.remaining_quantity = request.quantity;

    std::string rejection;
    if (request.id == 0) rejection = "Order ID must be greater than zero";
    else if (book_.has_order(request.id)) rejection = "Duplicate active Order ID";
    else if (!valid_side(request.side)) rejection = "Invalid order side";
    else if (!valid_type(request.type)) rejection = "Invalid order type";
    else if (request.quantity == 0 || request.quantity > MAX_ORDER_QUANTITY) {
        rejection = "Quantity must be between 1 and MAX_ORDER_QUANTITY";
    } else if (request.type == OrderType::Limit && request.price <= 0) {
        rejection = "Limit order price must be positive";
    } else if (request.type == OrderType::Limit && request.price > MAX_ORDER_PRICE) {
        rejection = "Limit order price exceeds MAX_ORDER_PRICE";
    }

    if (!rejection.empty()) {
        ++stats_.total_orders_rejected;
        result.status = OrderStatus::Rejected;
        result.sequence = ++next_sequence_;
        result.message = rejection;
        ActivityRecord record{};
        record.sequence = result.sequence;
        record.action = ActionType::Reject;
        record.order_id = request.id;
        record.side = request.side;
        record.order_type = request.type;
        record.price = request.price;
        record.quantity = request.quantity;
        record.status = result.status;
        record.message = rejection;
        append_activity(std::move(record));
        return result;
    }

    const SequenceNumber submit_sequence = ++next_sequence_;
    // Logical nanoseconds keep timestamps monotonic and replay-identical across runs.
    const TimestampNs timestamp = static_cast<TimestampNs>(submit_sequence);
    Order incoming{};
    incoming.id = request.id;
    incoming.side = request.side;
    incoming.type = request.type;
    incoming.price = request.type == OrderType::Market ? 0 : request.price;
    incoming.initial_quantity = request.quantity;
    incoming.remaining_quantity = request.quantity;
    incoming.sequence = submit_sequence;
    incoming.timestamp_ns = timestamp;
    ++stats_.total_orders_submitted;

    book_.match_against_book(incoming, next_sequence_, next_trade_id_, timestamp, result.trades);
    for (const Trade& trade : result.trades) append_trade(trade);

    std::vector<ActivityRecord> order_activities;
    order_activities.reserve(result.trades.size() + 1);

    if (incoming.remaining_quantity == 0) {
        result.status = OrderStatus::Filled;
    } else if (incoming.type == OrderType::Limit) {
        if (!book_.insert_resting_order(incoming)) {
            // Validation and matching should make this unreachable; fail closed if the book
            // cannot accept the remaining quantity rather than silently losing an order.
            result.status = OrderStatus::Rejected;
            result.message = "Could not add order remainder to the book";
            --stats_.total_orders_submitted;
            ++stats_.total_orders_rejected;
            result.sequence = submit_sequence;
            result.remaining_quantity = incoming.remaining_quantity;
            result.filled_quantity = incoming.filled_quantity();
            ActivityRecord rejected{};
            rejected.sequence = submit_sequence;
            rejected.action = ActionType::Reject;
            rejected.order_id = request.id;
            rejected.side = request.side;
            rejected.order_type = request.type;
            rejected.price = incoming.price;
            rejected.quantity = request.quantity;
            rejected.status = result.status;
            rejected.message = result.message;
            order_activities.push_back(std::move(rejected));
            for (const Trade& trade : result.trades) {
                ActivityRecord record{};
                record.sequence = trade.sequence;
                record.action = ActionType::Trade;
                record.order_id = trade.aggressor_order_id;
                record.side = trade.aggressor_side;
                record.order_type = incoming.type;
                record.price = trade.price;
                record.quantity = trade.quantity;
                record.status = OrderStatus::Filled;
                record.message = trade_message(trade);
                order_activities.push_back(std::move(record));
            }
            std::sort(order_activities.begin(), order_activities.end(),
                      [](const ActivityRecord& left, const ActivityRecord& right) {
                          return left.sequence < right.sequence;
                      });
            for (auto& record : order_activities) append_activity(std::move(record));
            refresh_book_stats();
            return result;
        }
        result.status = incoming.remaining_quantity < incoming.initial_quantity
            ? OrderStatus::PartiallyFilled : OrderStatus::New;
    } else {
        result.status = incoming.remaining_quantity < incoming.initial_quantity
            ? OrderStatus::PartiallyFilled : OrderStatus::Cancelled;
    }

    result.sequence = submit_sequence;
    result.filled_quantity = incoming.filled_quantity();
    result.remaining_quantity = incoming.remaining_quantity;
    result.message = "Order accepted";

    ActivityRecord submit{};
    submit.sequence = submit_sequence;
    submit.action = ActionType::Submit;
    submit.order_id = request.id;
    submit.side = request.side;
    submit.order_type = request.type;
    submit.price = incoming.price;
    submit.quantity = request.quantity;
    submit.status = result.status;
    submit.message = "Order accepted: " + std::string(to_string(result.status));
    order_activities.push_back(std::move(submit));
    for (const Trade& trade : result.trades) {
        ActivityRecord record{};
        record.sequence = trade.sequence;
        record.action = ActionType::Trade;
        record.order_id = trade.aggressor_order_id;
        record.side = trade.aggressor_side;
        record.order_type = incoming.type;
        record.price = trade.price;
        record.quantity = trade.quantity;
        record.status = OrderStatus::Filled;
        record.message = trade_message(trade);
        order_activities.push_back(std::move(record));
    }
    std::sort(order_activities.begin(), order_activities.end(),
              [](const ActivityRecord& left, const ActivityRecord& right) {
                  return left.sequence < right.sequence;
              });
    for (auto& record : order_activities) append_activity(std::move(record));
    refresh_book_stats();
    return result;
}

CancelResult MatchingEngine::cancel_order(const OrderId id) {
    CancelResult result = book_.cancel_order(id);
    if (!result.success) return result;

    result.sequence = ++next_sequence_;
    ++stats_.total_orders_cancelled;
    ActivityRecord record{};
    record.sequence = result.sequence;
    record.action = ActionType::Cancel;
    record.order_id = id;
    record.price = result.cancelled_price;
    record.quantity = result.cancelled_quantity;
    record.status = OrderStatus::Cancelled;
    record.message = result.message;
    append_activity(std::move(record));
    refresh_book_stats();
    return result;
}

BookSnapshot MatchingEngine::get_snapshot(const std::size_t depth) const {
    return book_.get_snapshot(depth);
}

EngineStats MatchingEngine::stats() const noexcept {
    EngineStats current = stats_;
    current.active_order_count = book_.active_order_count();
    current.bid_level_count = book_.bid_level_count();
    current.ask_level_count = book_.ask_level_count();
    return current;
}

std::vector<Trade> MatchingEngine::recent_trades(const std::size_t limit) const {
    const std::size_t count = std::min(limit, recent_trades_.size());
    std::vector<Trade> result;
    result.reserve(count);
    for (auto it = recent_trades_.rbegin(); it != recent_trades_.rend() && result.size() < count; ++it) {
        result.push_back(*it);
    }
    return result;
}

std::vector<ActivityRecord> MatchingEngine::recent_activity(const std::size_t limit) const {
    const std::size_t count = std::min(limit, recent_activity_.size());
    std::vector<ActivityRecord> result;
    result.reserve(count);
    for (auto it = recent_activity_.rbegin(); it != recent_activity_.rend() && result.size() < count; ++it) {
        result.push_back(*it);
    }
    return result;
}

void MatchingEngine::reset() noexcept {
    book_.clear();
    next_sequence_ = 0;
    next_trade_id_ = 0;
    next_auto_order_id_ = 1;
    stats_ = {};
    recent_trades_.clear();
    recent_activity_.clear();
}

void MatchingEngine::seed_demo_book() {
    if (book_.active_order_count() != 0) return;
    constexpr Price bid_prices[] = {10000, 9990, 9980, 9970, 9960};
    constexpr Price ask_prices[] = {10010, 10020, 10030, 10040, 10050};
    constexpr Quantity bid_quantities[] = {150, 220, 180, 260, 310};
    constexpr Quantity ask_quantities[] = {120, 200, 240, 190, 280};
    for (std::size_t i = 0; i < 5; ++i) {
        const OrderId bid_id = next_auto_order_id_++;
        const OrderId ask_id = next_auto_order_id_++;
        (void)submit_order(OrderRequest{bid_id, Side::Buy, OrderType::Limit,
                                        bid_prices[i], bid_quantities[i]});
        (void)submit_order(OrderRequest{ask_id, Side::Sell, OrderType::Limit,
                                        ask_prices[i], ask_quantities[i]});
    }
}

} // namespace basalt
