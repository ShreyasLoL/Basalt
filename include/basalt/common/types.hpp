#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace basalt::common {

// PriceTicks are discrete instrument ticks. MicroUnits are INR values at
// PRICE_SCALE micro-rupees per rupee; a tick size maps ticks to micro-units.
using PriceTicks = std::int64_t;
using MicroUnits = std::int64_t;
using Quantity = std::uint64_t;
using PositionQuantity = std::int64_t;
using InstrumentId = std::uint64_t;
using AccountId = std::uint64_t;
using StrategyId = std::uint64_t;
using EventSequence = std::uint64_t;

inline constexpr MicroUnits PRICE_SCALE = 10'000;

enum class DataSourceType : std::uint8_t {
    LiveProvider,
    DelayedProvider,
    Historical,
    Cached,
    Synthetic,
    Replayed,
    Derived
};

enum class FeedQualityState : std::uint8_t { Ok, Stale, Degraded, Disconnected };

struct Error {
    std::string code{};
    std::string message{};

    friend bool operator==(const Error&, const Error&) = default;
};

template <typename T, typename E = Error>
class Result {
public:
    [[nodiscard]] static Result success(T value) {
        return Result(std::in_place_index<0>, std::move(value));
    }
    [[nodiscard]] static Result failure(E error) {
        return Result(std::in_place_index<1>, std::move(error));
    }

    [[nodiscard]] bool has_value() const noexcept { return value_.index() == 0; }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
    [[nodiscard]] T& value() & { return std::get<0>(value_); }
    [[nodiscard]] const T& value() const& { return std::get<0>(value_); }
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(value_)); }
    [[nodiscard]] E& error() & { return std::get<1>(value_); }
    [[nodiscard]] const E& error() const& { return std::get<1>(value_); }

private:
    template <typename... Args>
    explicit Result(std::in_place_index_t<0> tag, Args&&... args)
        : value_(tag, std::forward<Args>(args)...) {}
    template <typename... Args>
    explicit Result(std::in_place_index_t<1> tag, Args&&... args)
        : value_(tag, std::forward<Args>(args)...) {}

    std::variant<T, E> value_;
};

template <typename E>
class Result<void, E> {
public:
    [[nodiscard]] static Result success() { return Result(true, E{}); }
    [[nodiscard]] static Result failure(E error) { return Result(false, std::move(error)); }

    [[nodiscard]] bool has_value() const noexcept { return success_; }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
    [[nodiscard]] E& error() & { return error_; }
    [[nodiscard]] const E& error() const& { return error_; }

private:
    Result(const bool success, E error) : success_(success), error_(std::move(error)) {}
    bool success_;
    E error_;
};

[[nodiscard]] inline Result<std::int64_t> checked_add(const std::int64_t left,
                                                       const std::int64_t right) {
    constexpr std::int64_t minimum = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t maximum = std::numeric_limits<std::int64_t>::max();
    if ((right > 0 && left > maximum - right) || (right < 0 && left < minimum - right)) {
        return Result<std::int64_t>::failure(
            Error{"ARITHMETIC_OVERFLOW", "Signed 64-bit addition overflow"});
    }
    return Result<std::int64_t>::success(left + right);
}

[[nodiscard]] inline Result<std::int64_t> checked_multiply(const std::int64_t left,
                                                            const std::int64_t right) {
    constexpr std::int64_t minimum = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t maximum = std::numeric_limits<std::int64_t>::max();
    if (left == 0 || right == 0) return Result<std::int64_t>::success(0);

    bool overflow = false;
    if (left > 0) {
        overflow = right > 0 ? left > maximum / right : right < minimum / left;
    } else {
        overflow = right > 0 ? left < minimum / right : right < maximum / left;
    }
    if (overflow) {
        return Result<std::int64_t>::failure(
            Error{"ARITHMETIC_OVERFLOW", "Signed 64-bit multiplication overflow"});
    }
    return Result<std::int64_t>::success(left * right);
}

[[nodiscard]] constexpr std::string_view to_string(const DataSourceType value) noexcept {
    switch (value) {
    case DataSourceType::LiveProvider: return "LIVE_PROVIDER";
    case DataSourceType::DelayedProvider: return "DELAYED_PROVIDER";
    case DataSourceType::Historical: return "HISTORICAL";
    case DataSourceType::Cached: return "CACHED";
    case DataSourceType::Synthetic: return "SYNTHETIC";
    case DataSourceType::Replayed: return "REPLAYED";
    case DataSourceType::Derived: return "DERIVED";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr std::string_view to_string(const FeedQualityState value) noexcept {
    switch (value) {
    case FeedQualityState::Ok: return "OK";
    case FeedQualityState::Stale: return "STALE";
    case FeedQualityState::Degraded: return "DEGRADED";
    case FeedQualityState::Disconnected: return "DISCONNECTED";
    }
    return "UNKNOWN";
}

} // namespace basalt::common
