#pragma once

#include <cstdint>
#include <string_view>

namespace atlas {

using OrderId = std::uint64_t;
using TradeId = std::uint64_t;
using Price = std::int64_t;
using Quantity = std::uint64_t;
using SequenceNumber = std::uint64_t;
using TimestampNs = std::uint64_t;

inline constexpr Price MAX_ORDER_PRICE = 1'000'000'000'000LL;
inline constexpr Quantity MAX_ORDER_QUANTITY = 1'000'000'000ULL;
inline constexpr std::size_t HISTORY_CAPACITY = 200;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };
enum class OrderType : std::uint8_t { Limit = 0, Market = 1 };
enum class OrderStatus : std::uint8_t {
    New = 0,
    PartiallyFilled = 1,
    Filled = 2,
    Cancelled = 3,
    Rejected = 4
};
enum class ActionType : std::uint8_t { Submit = 0, Cancel = 1, Trade = 2, Reject = 3 };

[[nodiscard]] std::string_view to_string(Side value) noexcept;
[[nodiscard]] std::string_view to_string(OrderType value) noexcept;
[[nodiscard]] std::string_view to_string(OrderStatus value) noexcept;
[[nodiscard]] std::string_view to_string(ActionType value) noexcept;

} // namespace atlas
