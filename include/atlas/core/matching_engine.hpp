#pragma once

#include "atlas/core/order_book.hpp"

#include <deque>
#include <string>
#include <vector>

namespace atlas {

class MatchingEngine {
public:
    MatchingEngine() = default;

    [[nodiscard]] OrderResult submit_order(const OrderRequest& request);
    [[nodiscard]] CancelResult cancel_order(OrderId id);
    [[nodiscard]] BookSnapshot get_snapshot(std::size_t depth = 20) const;
    [[nodiscard]] EngineStats stats() const noexcept;
    [[nodiscard]] std::vector<Trade> recent_trades(std::size_t limit = 50) const;
    [[nodiscard]] std::vector<ActivityRecord> recent_activity(std::size_t limit = 50) const;
    void reset() noexcept;
    void seed_demo_book();

    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }

private:
    OrderBook book_{};
    SequenceNumber next_sequence_{0};
    TradeId next_trade_id_{0};
    OrderId next_auto_order_id_{1};
    EngineStats stats_{};
    std::deque<Trade> recent_trades_{};
    std::deque<ActivityRecord> recent_activity_{};
    void append_trade(const Trade& trade);
    void append_activity(ActivityRecord record);
    void refresh_book_stats() noexcept;
};

} // namespace atlas
