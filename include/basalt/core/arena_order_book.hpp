#pragma once

#include "basalt/core/object_pool.hpp"
#include "basalt/core/trade.hpp"

#include <cstddef>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace basalt {

class ArenaOrderBook {
public:
    explicit ArenaOrderBook(std::size_t order_capacity);

    [[nodiscard]] bool has_order(OrderId id) const noexcept;
    [[nodiscard]] const Order* get_order(OrderId id) const noexcept;
    [[nodiscard]] std::size_t active_order_count() const noexcept { return locations_.size(); }
    [[nodiscard]] std::size_t pool_capacity() const noexcept { return pool_.capacity(); }
    [[nodiscard]] std::size_t pool_available() const noexcept { return pool_.available(); }
    [[nodiscard]] std::uint64_t pool_allocations() const noexcept { return pool_.acquisitions(); }
    [[nodiscard]] std::size_t pool_peak_active() const noexcept { return pool_.high_water(); }
    [[nodiscard]] std::string_view last_error() const noexcept { return last_error_; }
    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;
    [[nodiscard]] bool insert_resting_order(const Order& order);
    [[nodiscard]] CancelResult cancel_order(OrderId id);
    void match_against_book(Order& incoming, SequenceNumber& seq_counter,
                            TradeId& trade_id_counter, TimestampNs now,
                            std::vector<Trade>& out_trades);
    [[nodiscard]] BookSnapshot get_snapshot(std::size_t depth = 20) const;
    [[nodiscard]] bool verify_invariants(std::string* error_out = nullptr) const;
    void clear() noexcept;

private:
    using Index = std::size_t;
    static constexpr Index no_index = std::numeric_limits<Index>::max();
    struct Link { PoolHandle handle{}; Index previous{no_index}; Index next{no_index}; };
    struct Level { Index head{no_index}; Index tail{no_index}; Quantity total_volume{}; };
    using BidLevels = std::map<Price, Level, std::greater<Price>>;
    using AskLevels = std::map<Price, Level, std::less<Price>>;

    ObjectPool<Order> pool_;
    std::vector<Link> links_{};
    BidLevels bids_{};
    AskLevels asks_{};
    std::unordered_map<OrderId, PoolHandle> locations_{};
    std::string last_error_{};

    [[nodiscard]] bool link_order(const Order& order);
    void unlink_order(Index index, Level& level) noexcept;
    [[nodiscard]] Index index_for(OrderId id) const noexcept;
};

} // namespace basalt
