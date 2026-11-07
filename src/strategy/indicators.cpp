#include "basalt/strategy/indicators.hpp"

#include "basalt/common/fixed_math.hpp"

#include <limits>

namespace basalt::strategy {
namespace {

common::Result<std::int64_t> subtract(const std::int64_t left, const std::int64_t right) {
    if ((right > 0 && left < std::numeric_limits<std::int64_t>::min() + right) ||
        (right < 0 && left > std::numeric_limits<std::int64_t>::max() + right)) {
        return common::Result<std::int64_t>::failure(
            {"ARITHMETIC_OVERFLOW", "Signed subtraction overflow"});
    }
    return common::Result<std::int64_t>::success(left - right);
}

std::optional<std::uint64_t> sum_quantities(const std::uint64_t left,
                                           const std::uint64_t right) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) return std::nullopt;
    return left + right;
}

std::uint64_t integer_sqrt(const std::uint64_t value) noexcept {
    std::uint64_t low = 0;
    std::uint64_t high = 3'037'000'500ULL;
    while (low < high) {
        const auto middle = low + (high - low + 1) / 2;
        if (middle == 0 || middle <= value / middle) low = middle;
        else high = middle - 1;
    }
    return low;
}

} // namespace

SimpleMovingAverage::SimpleMovingAverage(const std::size_t period) : period_(period) {}

common::Result<void> SimpleMovingAverage::update(const std::int64_t value) {
    if (period_ == 0) return common::Result<void>::failure({"INVALID_PERIOD", "SMA period must be positive"});
    auto candidate = common::Result<std::int64_t>::success(sum_);
    if (values_.size() == period_) {
        auto reduced = subtract(sum_, values_.front());
        if (!reduced) return common::Result<void>::failure(reduced.error());
        candidate = std::move(reduced);
    }
    auto next = common::checked_add(candidate.value(), value);
    if (!next) return common::Result<void>::failure(next.error());
    values_.push_back(value);
    if (values_.size() > period_) values_.pop_front();
    sum_ = next.value();
    return common::Result<void>::success();
}

std::optional<std::int64_t> SimpleMovingAverage::value() const noexcept {
    if (period_ == 0 || values_.size() < period_) return std::nullopt;
    return sum_ / static_cast<std::int64_t>(period_);
}

ExponentialMovingAverage::ExponentialMovingAverage(const std::size_t period) : period_(period) {}

common::Result<void> ExponentialMovingAverage::update(const std::int64_t value) {
    if (period_ == 0 || period_ >= static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
        return common::Result<void>::failure({"INVALID_PERIOD", "EMA period is outside the supported range"});
    if (!value_) value_ = value;
    else {
        auto difference = subtract(value, *value_);
        if (!difference) return common::Result<void>::failure(difference.error());
        const auto divisor = static_cast<std::int64_t>(period_ + 1);
        const auto quotient = difference.value() / divisor;
        const auto remainder = difference.value() % divisor;
        auto main_step = common::checked_multiply(quotient, 2);
        if (!main_step) return common::Result<void>::failure(main_step.error());
        auto residual = common::checked_multiply(remainder, 2);
        if (!residual) return common::Result<void>::failure(residual.error());
        auto step = common::checked_add(main_step.value(), residual.value() / divisor);
        if (!step) return common::Result<void>::failure(step.error());
        auto next = common::checked_add(*value_, step.value());
        if (!next) return common::Result<void>::failure(next.error());
        value_ = next.value();
    }
    ++samples_;
    return common::Result<void>::success();
}

RelativeStrengthIndex::RelativeStrengthIndex(const std::size_t period) : period_(period) {}

common::Result<void> RelativeStrengthIndex::update(const std::int64_t close) {
    if (period_ == 0 || period_ >= static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
        return common::Result<void>::failure({"INVALID_PERIOD", "RSI period is outside the supported range"});
    if (!previous_) {
        previous_ = close;
        return common::Result<void>::success();
    }
    auto change = subtract(close, *previous_);
    if (!change) return common::Result<void>::failure(change.error());
    const auto gain = change.value() > 0 ? change.value() : 0;
    if (change.value() == std::numeric_limits<std::int64_t>::min())
        return common::Result<void>::failure({"ARITHMETIC_OVERFLOW", "RSI loss magnitude overflow"});
    const auto loss = change.value() < 0 ? -change.value() : 0;
    previous_ = close;
    if (samples_ < period_) {
        auto new_gain = common::checked_add(gain_sum_, gain);
        auto new_loss = common::checked_add(loss_sum_, loss);
        if (!new_gain) return common::Result<void>::failure(new_gain.error());
        if (!new_loss) return common::Result<void>::failure(new_loss.error());
        gain_sum_ = new_gain.value();
        loss_sum_ = new_loss.value();
        ++samples_;
        if (samples_ == period_) {
            average_gain_ = gain_sum_ / static_cast<std::int64_t>(period_);
            average_loss_ = loss_sum_ / static_cast<std::int64_t>(period_);
        }
    } else {
        const auto divisor = static_cast<std::int64_t>(period_);
        auto weighted_gain = common::checked_multiply(average_gain_, divisor - 1);
        auto weighted_loss = common::checked_multiply(average_loss_, divisor - 1);
        if (!weighted_gain) return common::Result<void>::failure(weighted_gain.error());
        if (!weighted_loss) return common::Result<void>::failure(weighted_loss.error());
        auto next_gain = common::checked_add(weighted_gain.value(), gain);
        auto next_loss = common::checked_add(weighted_loss.value(), loss);
        if (!next_gain) return common::Result<void>::failure(next_gain.error());
        if (!next_loss) return common::Result<void>::failure(next_loss.error());
        average_gain_ = next_gain.value() / divisor;
        average_loss_ = next_loss.value() / divisor;
    }
    if (samples_ >= period_) {
        if (average_gain_ == 0 && average_loss_ == 0) value_bps_ = 5'000;
        else if (average_loss_ == 0) value_bps_ = 10'000;
        else if (average_gain_ == 0) value_bps_ = 0;
        else {
            auto total = common::checked_add(average_gain_, average_loss_);
            if (!total || total.value() <= 0)
                return common::Result<void>::failure({"ARITHMETIC_OVERFLOW", "RSI total movement overflow"});
            value_bps_ = static_cast<std::int64_t>(common::clamped_ratio_bps(
                static_cast<std::uint64_t>(average_gain_), static_cast<std::uint64_t>(total.value())));
        }
    }
    return common::Result<void>::success();
}

BollingerBandsIndicator::BollingerBandsIndicator(const std::size_t period,
                                                 const std::uint32_t deviation_bps)
    : period_(period), deviation_bps_(deviation_bps) {}

common::Result<void> BollingerBandsIndicator::update(const std::int64_t close) {
    if (period_ == 0 || period_ > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) ||
        deviation_bps_ == 0)
        return common::Result<void>::failure({"INVALID_PERIOD", "Bollinger settings must be positive"});
    auto values = values_;
    values.push_back(close);
    if (values.size() > period_) values.pop_front();
    if (values.size() < period_) {
        values_ = std::move(values);
        value_.reset();
        return common::Result<void>::success();
    }
    std::int64_t sum = 0;
    for (const auto item : values) {
        auto next = common::checked_add(sum, item);
        if (!next) return common::Result<void>::failure(next.error());
        sum = next.value();
    }
    const auto middle = sum / static_cast<std::int64_t>(period_);
    std::int64_t square_sum = 0;
    for (const auto item : values) {
        auto deviation = subtract(item, middle);
        if (!deviation) return common::Result<void>::failure(deviation.error());
        auto square = common::checked_multiply(deviation.value(), deviation.value());
        if (!square) return common::Result<void>::failure(square.error());
        auto next = common::checked_add(square_sum, square.value());
        if (!next) return common::Result<void>::failure(next.error());
        square_sum = next.value();
    }
    const auto variance = static_cast<std::uint64_t>(square_sum / static_cast<std::int64_t>(period_));
    const auto standard_deviation = static_cast<std::int64_t>(integer_sqrt(variance));
    auto scaled = common::checked_multiply(standard_deviation, static_cast<std::int64_t>(deviation_bps_));
    if (!scaled) return common::Result<void>::failure(scaled.error());
    const auto width = scaled.value() / 10'000;
    auto upper = common::checked_add(middle, width);
    auto lower = subtract(middle, width);
    if (!upper) return common::Result<void>::failure(upper.error());
    if (!lower) return common::Result<void>::failure(lower.error());
    values_ = std::move(values);
    value_ = BollingerBands{middle, upper.value(), lower.value()};
    return common::Result<void>::success();
}

common::Result<void> SessionVwap::update(const std::int64_t price, const std::uint64_t volume,
                                         std::string session_key) {
    if (price <= 0 || volume == 0 || volume > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        session_key.empty())
        return common::Result<void>::failure({"INVALID_VWAP_INPUT", "VWAP needs positive price, volume and session key"});
    const bool new_session = session_key_ != session_key;
    const auto old_notional = new_session ? 0 : notional_;
    const auto old_volume = new_session ? 0ULL : volume_;
    auto trade_notional = common::checked_multiply(price, static_cast<std::int64_t>(volume));
    if (!trade_notional) return common::Result<void>::failure(trade_notional.error());
    auto notional = common::checked_add(old_notional, trade_notional.value());
    if (!notional) return common::Result<void>::failure(notional.error());
    auto total_volume = sum_quantities(old_volume, volume);
    if (!total_volume || *total_volume > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return common::Result<void>::failure({"ARITHMETIC_OVERFLOW", "VWAP accumulated volume overflow"});
    session_key_ = std::move(session_key);
    notional_ = notional.value();
    volume_ = *total_volume;
    value_ = notional_ / static_cast<std::int64_t>(volume_);
    return common::Result<void>::success();
}

std::optional<std::int64_t> BookImbalance::value_bps(const std::uint64_t bid_quantity,
                                                     const std::uint64_t ask_quantity) const noexcept {
    if (levels_ == 0 || bid_quantity > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        ask_quantity > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
    auto total = sum_quantities(bid_quantity, ask_quantity);
    if (!total || *total == 0 || *total > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return std::nullopt;
    const auto positive = bid_quantity >= ask_quantity;
    const auto difference = positive ? bid_quantity - ask_quantity : ask_quantity - bid_quantity;
    const auto magnitude = static_cast<std::int64_t>(common::clamped_ratio_bps(difference, *total));
    return positive ? magnitude : -magnitude;
}

} // namespace basalt::strategy
