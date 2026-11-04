#pragma once

#include "atlas/core/types.hpp"

#include <cstddef>
#include <list>

namespace atlas {

struct OrderRequest {
    OrderId id{0};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    Price price{0};
    Quantity quantity{0};
};

struct Order {
    OrderId id{0};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    Price price{0};
    Quantity initial_quantity{0};
    Quantity remaining_quantity{0};
    SequenceNumber sequence{0};
    TimestampNs timestamp_ns{0};

    [[nodiscard]] Quantity filled_quantity() const noexcept {
        return initial_quantity - remaining_quantity;
    }
    [[nodiscard]] bool is_filled() const noexcept { return remaining_quantity == 0; }

    friend bool operator==(const Order&, const Order&) = default;
};

struct PriceLevel {
    Price price{0};
    Quantity total_volume{0};
    std::list<Order> orders{};

    [[nodiscard]] bool empty() const noexcept { return orders.empty(); }
    [[nodiscard]] std::size_t order_count() const noexcept { return orders.size(); }
};

struct OrderLocation {
    Side side{Side::Buy};
    Price price{0};
    std::list<Order>::iterator iterator{};
};

} // namespace atlas
