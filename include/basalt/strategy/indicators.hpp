#pragma once

#include "basalt/common/types.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace basalt::strategy {

class SimpleMovingAverage {
public:
    explicit SimpleMovingAverage(std::size_t period);
    [[nodiscard]] common::Result<void> update(std::int64_t value);
    [[nodiscard]] std::optional<std::int64_t> value() const noexcept;
    [[nodiscard]] std::size_t samples() const noexcept { return values_.size(); }
private:
    std::size_t period_;
    std::deque<std::int64_t> values_{};
    std::int64_t sum_{};
};

class ExponentialMovingAverage {
public:
    explicit ExponentialMovingAverage(std::size_t period);
    [[nodiscard]] common::Result<void> update(std::int64_t value);
    [[nodiscard]] std::optional<std::int64_t> value() const noexcept {
        return samples_ >= period_ ? value_ : std::nullopt;
    }
    [[nodiscard]] std::size_t samples() const noexcept { return samples_; }
private:
    std::size_t period_;
    std::size_t samples_{};
    std::optional<std::int64_t> value_{};
};

class RelativeStrengthIndex {
public:
    explicit RelativeStrengthIndex(std::size_t period);
    // Returns basis points of the 0–100 RSI scale (7000 means RSI 70.00).
    [[nodiscard]] common::Result<void> update(std::int64_t close);
    [[nodiscard]] std::optional<std::int64_t> value_bps() const noexcept { return value_bps_; }
private:
    std::size_t period_;
    std::size_t samples_{};
    std::optional<std::int64_t> previous_{};
    std::int64_t gain_sum_{};
    std::int64_t loss_sum_{};
    std::int64_t average_gain_{};
    std::int64_t average_loss_{};
    std::optional<std::int64_t> value_bps_{};
};

struct BollingerBands {
    std::int64_t middle{};
    std::int64_t upper{};
    std::int64_t lower{};
};

class BollingerBandsIndicator {
public:
    BollingerBandsIndicator(std::size_t period, std::uint32_t deviation_bps = 20'000);
    [[nodiscard]] common::Result<void> update(std::int64_t close);
    [[nodiscard]] std::optional<BollingerBands> value() const noexcept { return value_; }
private:
    std::size_t period_;
    std::uint32_t deviation_bps_;
    std::deque<std::int64_t> values_{};
    std::optional<BollingerBands> value_{};
};

class SessionVwap {
public:
    [[nodiscard]] common::Result<void> update(std::int64_t price, std::uint64_t volume,
                                              std::string session_key);
    [[nodiscard]] std::optional<std::int64_t> value() const noexcept { return value_; }
    [[nodiscard]] const std::string& session_key() const noexcept { return session_key_; }
private:
    std::string session_key_{};
    std::int64_t notional_{};
    std::uint64_t volume_{};
    std::optional<std::int64_t> value_{};
};

class BookImbalance {
public:
    explicit BookImbalance(std::size_t levels = 5) : levels_(levels) {}
    [[nodiscard]] std::optional<std::int64_t> value_bps(
        std::uint64_t bid_quantity, std::uint64_t ask_quantity) const noexcept;
    [[nodiscard]] std::size_t levels() const noexcept { return levels_; }
private:
    std::size_t levels_;
};

} // namespace basalt::strategy
