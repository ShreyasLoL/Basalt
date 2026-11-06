#include "basalt/strategy/builtins.hpp"

#include "basalt/strategy/indicators.hpp"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace basalt::strategy {
namespace {

enum class BollingerMode : std::uint8_t { Breakout, Reversion };

std::uint64_t stable_hash(const std::string& input) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char character : input) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hash_string(const std::string& input) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << stable_hash(input);
    return output.str();
}

common::Result<std::int64_t> ceil_mul_div(const std::int64_t value, const std::uint32_t multiplier,
                                          const std::int64_t divisor) {
    if (value < 0 || divisor <= 0)
        return common::Result<std::int64_t>::failure({"INVALID_FIXED_POINT_INPUT", "Expected nonnegative numerator"});
    auto product = common::checked_multiply(value, static_cast<std::int64_t>(multiplier));
    if (!product) return product;
    const auto quotient = product.value() / divisor;
    const auto remainder = product.value() % divisor;
    return common::Result<std::int64_t>::success(quotient + (remainder == 0 ? 0 : 1));
}

std::optional<std::int64_t> ratio_bps(const std::uint64_t numerator,
                                      const std::uint64_t denominator) noexcept {
    if (denominator == 0) return std::nullopt;
    const auto whole = numerator / denominator;
    if (whole > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 10'000))
        return std::numeric_limits<std::int64_t>::max();
    const auto remainder = numerator % denominator;
    // Compute floor(remainder*10,000/denominator) without an overflowing product.
    std::uint64_t carry = 0;
    std::uint64_t fraction = 0;
    const auto complement = denominator - remainder;
    for (std::uint32_t i = 0; i < 10'000; ++i) {
        if (carry >= complement) {
            carry -= complement;
            ++fraction;
        } else {
            carry += remainder;
        }
    }
    return static_cast<std::int64_t>(whole * 10'000 + fraction);
}

class BuiltinStrategy final : public IStrategy {
public:
    explicit BuiltinStrategy(const BuiltinKind kind) : kind_(kind) {}

    StrategyDescriptor descriptor() const override {
        StrategyDescriptor value{};
        value.strategy_id = std::string(to_string(kind_));
        value.name = value.strategy_id;
        value.version = "1.0.0";
        value.stable_hash = hash_string(value.strategy_id + ":1.0.0:fixed-point-v1");
        value.required_channels = {kind_ == BuiltinKind::OrderBookImbalance
                                       ? market_data::MarketEventKind::Depth
                                       : market_data::MarketEventKind::Bar};
        switch (kind_) {
        case BuiltinKind::SmaCrossover: value.required_warmup_events = slow_period_; break;
        case BuiltinKind::RsiMeanReversion: value.required_warmup_events = period_ + 1; break;
        case BuiltinKind::Bollinger: value.required_warmup_events = period_; break;
        case BuiltinKind::VwapReversion: value.required_warmup_events = 2; break;
        case BuiltinKind::MomentumBreakout:
            value.required_warmup_events = std::max(period_, volume_period_) + 1;
            break;
        case BuiltinKind::OrderBookImbalance: value.required_warmup_events = 1; break;
        }
        switch (kind_) {
        case BuiltinKind::SmaCrossover:
            value.parameter_schema = {{"fast_period", StrategyParameterType::Integer, 1, 99'999, "5"},
                                      {"slow_period", StrategyParameterType::Integer, 2, 100'000, "20"}};
            break;
        case BuiltinKind::RsiMeanReversion:
            value.parameter_schema = {{"period", StrategyParameterType::Integer, 1, 100'000, "14"},
                                      {"oversold_bps", StrategyParameterType::Integer, 1, 4'999, "3000"},
                                      {"neutral_bps", StrategyParameterType::Integer, 1, 9'999, "5000"},
                                      {"overbought_bps", StrategyParameterType::Integer, 5'001, 9'999, "7000"}};
            break;
        case BuiltinKind::Bollinger:
            value.parameter_schema = {{"period", StrategyParameterType::Integer, 1, 100'000, "20"},
                                      {"deviation_bps", StrategyParameterType::Integer, 1, 100'000, "20000"},
                                      {"minimum_width_bps", StrategyParameterType::Integer, 0, 100'000, "0"},
                                      {"mode", StrategyParameterType::Text, 0, 0, "reversion|breakout"}};
            break;
        case BuiltinKind::VwapReversion:
            value.parameter_schema = {{"deviation_threshold_bps", StrategyParameterType::Integer, 1, 100'000, "100"},
                                      {"exit_buffer_bps", StrategyParameterType::Integer, 0, 100'000, "10"}};
            break;
        case BuiltinKind::MomentumBreakout:
            value.parameter_schema = {{"period", StrategyParameterType::Integer, 1, 100'000, "20"},
                                      {"volume_period", StrategyParameterType::Integer, 1, 100'000, "20"},
                                      {"volume_factor_bps", StrategyParameterType::Integer, 1, 100'000, "10000"}};
            break;
        case BuiltinKind::OrderBookImbalance:
            value.parameter_schema = {{"threshold_bps", StrategyParameterType::Integer, 1, 10'000, "1500"}};
            break;
        }
        return value;
    }

    common::Result<void, StrategyError> configure(const StrategyConfig& config) override {
        if (config.order_quantity == 0 || config.sizing_budget_micro < 0 ||
            config.fee_bps > 100'000 || config.slippage_bps > 100'000)
            return common::Result<void, StrategyError>::failure(
                {"INVALID_STRATEGY_CONFIG", "Order size, budget, fee or slippage is invalid"});
        config_ = config;
        auto parsed = parse_parameters(config);
        if (!parsed) return parsed;
        switch (kind_) {
        case BuiltinKind::SmaCrossover:
            fast_sma_ = std::make_unique<SimpleMovingAverage>(fast_period_);
            slow_sma_ = std::make_unique<SimpleMovingAverage>(slow_period_);
            break;
        case BuiltinKind::RsiMeanReversion: rsi_ = std::make_unique<RelativeStrengthIndex>(period_); break;
        case BuiltinKind::Bollinger:
            bands_ = std::make_unique<BollingerBandsIndicator>(period_, deviation_bps_);
            break;
        case BuiltinKind::VwapReversion: vwap_ = std::make_unique<SessionVwap>(); break;
        case BuiltinKind::MomentumBreakout: break;
        case BuiltinKind::OrderBookImbalance: imbalance_ = std::make_unique<BookImbalance>(5); break;
        }
        configured_ = true;
        return common::Result<void, StrategyError>::success();
    }

    common::Result<void, StrategyError> initialise(const StrategyContext& context) override {
        if (!configured_ || context.instrument.id == 0 || context.instrument.tick_size_micro <= 0 ||
            context.instrument.lot_size == 0)
            return common::Result<void, StrategyError>::failure(
                {"INVALID_STRATEGY_CONTEXT", "Strategy needs a configured instrument with tick and lot sizes"});
        return common::Result<void, StrategyError>::success();
    }

    common::Result<std::vector<StrategySignal>, StrategyError> on_event(
        const market_data::CanonicalMarketEvent& event, const StrategyContext& context) override {
        if (!configured_) return common::Result<std::vector<StrategySignal>, StrategyError>::failure(
            {"STRATEGY_NOT_CONFIGURED", "Configure the built-in strategy before dispatch"});
        auto effective_context = context;
        effective_context.order_quantity = config_.order_quantity;
        effective_context.sizing_budget_micro = config_.sizing_budget_micro;
        effective_context.fee_bps = config_.fee_bps;
        effective_context.slippage_bps = config_.slippage_bps;
        std::vector<StrategySignal> output;
        if (kind_ == BuiltinKind::OrderBookImbalance) {
            auto signal = on_depth(event, effective_context);
            if (!signal) return common::Result<std::vector<StrategySignal>, StrategyError>::failure(signal.error());
            if (signal.value()) output.push_back(std::move(*signal.value()));
            return common::Result<std::vector<StrategySignal>, StrategyError>::success(std::move(output));
        }
        if (event.kind != market_data::MarketEventKind::Bar || !event.bar) return
            common::Result<std::vector<StrategySignal>, StrategyError>::success(std::move(output));
        const auto close = event.bar->close_micro;
        const auto volume = event.bar->volume;
        if (close <= 0) return common::Result<std::vector<StrategySignal>, StrategyError>::success({});
        auto signal = on_bar(event, effective_context, close, volume);
        if (!signal) return common::Result<std::vector<StrategySignal>, StrategyError>::failure(signal.error());
        if (signal.value()) output.push_back(std::move(*signal.value()));
        return common::Result<std::vector<StrategySignal>, StrategyError>::success(std::move(output));
    }

    void on_execution(const trading::ExecutionReport&, const StrategyContext&) override {}
    void on_stop() noexcept override { stopped_ = true; }

private:
    common::Result<void, StrategyError> parse_parameters(const StrategyConfig& config) {
        auto get = [&config](const std::string& key, const std::int64_t fallback,
                             const std::int64_t minimum, const std::int64_t maximum)
            -> common::Result<std::int64_t, StrategyError> {
            const auto found = config.integer_parameters.find(key);
            const auto value = found == config.integer_parameters.end() ? fallback : found->second;
            if (value < minimum || value > maximum)
                return common::Result<std::int64_t, StrategyError>::failure(
                    {"INVALID_STRATEGY_CONFIG", "Parameter " + key + " is outside its allowed range"});
            return common::Result<std::int64_t, StrategyError>::success(value);
        };
        auto allow_only = [&config](const std::vector<std::string>& integers,
                                    const std::vector<std::string>& strings)
            -> common::Result<void, StrategyError> {
            for (const auto& [key, _] : config.integer_parameters)
                if (std::find(integers.begin(), integers.end(), key) == integers.end())
                    return common::Result<void, StrategyError>::failure(
                        {"UNKNOWN_STRATEGY_PARAMETER", "Unknown integer strategy parameter: " + key});
            for (const auto& [key, _] : config.text_parameters)
                if (std::find(strings.begin(), strings.end(), key) == strings.end())
                    return common::Result<void, StrategyError>::failure(
                        {"UNKNOWN_STRATEGY_PARAMETER", "Unknown text strategy parameter: " + key});
            return common::Result<void, StrategyError>::success();
        };
        auto period = [&](const std::string& key, std::int64_t fallback) {
            return get(key, fallback, 1, 100'000);
        };
        auto set_period = [&](const std::string& key, std::int64_t fallback) -> common::Result<void, StrategyError> {
            auto value = period(key, fallback);
            if (!value) return common::Result<void, StrategyError>::failure(value.error());
            period_ = static_cast<std::size_t>(value.value());
            return common::Result<void, StrategyError>::success();
        };
        common::Result<void, StrategyError> allowed = common::Result<void, StrategyError>::success();
        switch (kind_) {
        case BuiltinKind::SmaCrossover: {
            allowed = allow_only({"fast_period", "slow_period"}, {});
            if (!allowed) return allowed;
            auto fast = period("fast_period", 5);
            auto slow = period("slow_period", 20);
            if (!fast) return common::Result<void, StrategyError>::failure(fast.error());
            if (!slow) return common::Result<void, StrategyError>::failure(slow.error());
            if (fast.value() >= slow.value()) return common::Result<void, StrategyError>::failure(
                {"INVALID_STRATEGY_CONFIG", "SMA fast_period must be smaller than slow_period"});
            fast_period_ = static_cast<std::size_t>(fast.value());
            slow_period_ = static_cast<std::size_t>(slow.value());
            period_ = slow_period_;
            break;
        }
        case BuiltinKind::RsiMeanReversion: {
            allowed = allow_only({"period", "oversold_bps", "overbought_bps", "neutral_bps"}, {});
            if (!allowed) return allowed;
            auto parsed = set_period("period", 14);
            if (!parsed) return parsed;
            auto oversold = get("oversold_bps", 3'000, 1, 4'999);
            auto overbought = get("overbought_bps", 7'000, 5'001, 9'999);
            auto neutral = get("neutral_bps", 5'000, 1, 9'999);
            if (!oversold) return common::Result<void, StrategyError>::failure(oversold.error());
            if (!overbought) return common::Result<void, StrategyError>::failure(overbought.error());
            if (!neutral) return common::Result<void, StrategyError>::failure(neutral.error());
            if (oversold.value() >= neutral.value() || neutral.value() >= overbought.value())
                return common::Result<void, StrategyError>::failure(
                    {"INVALID_STRATEGY_CONFIG", "RSI thresholds must satisfy oversold < neutral < overbought"});
            oversold_bps_ = oversold.value();
            overbought_bps_ = overbought.value();
            neutral_bps_ = neutral.value();
            break;
        }
        case BuiltinKind::Bollinger: {
            allowed = allow_only({"period", "deviation_bps", "minimum_width_bps"}, {"mode"});
            if (!allowed) return allowed;
            auto parsed = set_period("period", 20);
            if (!parsed) return parsed;
            auto deviation = get("deviation_bps", 20'000, 1, 100'000);
            auto minimum_width = get("minimum_width_bps", 0, 0, 100'000);
            if (!deviation) return common::Result<void, StrategyError>::failure(deviation.error());
            if (!minimum_width) return common::Result<void, StrategyError>::failure(minimum_width.error());
            deviation_bps_ = static_cast<std::uint32_t>(deviation.value());
            minimum_width_bps_ = minimum_width.value();
            const auto mode = config.text_parameters.find("mode");
            const auto mode_value = mode == config.text_parameters.end() ? "reversion" : mode->second;
            if (mode_value == "breakout") bollinger_mode_ = BollingerMode::Breakout;
            else if (mode_value == "reversion") bollinger_mode_ = BollingerMode::Reversion;
            else return common::Result<void, StrategyError>::failure(
                {"INVALID_STRATEGY_CONFIG", "Bollinger mode must be breakout or reversion"});
            break;
        }
        case BuiltinKind::VwapReversion: {
            allowed = allow_only({"deviation_threshold_bps", "exit_buffer_bps"}, {});
            if (!allowed) return allowed;
            auto threshold = get("deviation_threshold_bps", 100, 1, 100'000);
            auto exit = get("exit_buffer_bps", 10, 0, 100'000);
            if (!threshold) return common::Result<void, StrategyError>::failure(threshold.error());
            if (!exit) return common::Result<void, StrategyError>::failure(exit.error());
            deviation_threshold_bps_ = threshold.value();
            exit_buffer_bps_ = exit.value();
            break;
        }
        case BuiltinKind::MomentumBreakout: {
            allowed = allow_only({"period", "volume_period", "volume_factor_bps"}, {});
            if (!allowed) return allowed;
            auto channel = period("period", 20);
            auto volume_period = period("volume_period", 20);
            auto factor = get("volume_factor_bps", 10'000, 1, 100'000);
            if (!channel) return common::Result<void, StrategyError>::failure(channel.error());
            if (!volume_period) return common::Result<void, StrategyError>::failure(volume_period.error());
            if (!factor) return common::Result<void, StrategyError>::failure(factor.error());
            period_ = static_cast<std::size_t>(channel.value());
            volume_period_ = static_cast<std::size_t>(volume_period.value());
            volume_factor_bps_ = static_cast<std::uint32_t>(factor.value());
            break;
        }
        case BuiltinKind::OrderBookImbalance: {
            allowed = allow_only({"threshold_bps"}, {});
            if (!allowed) return allowed;
            auto threshold = get("threshold_bps", 1'500, 1, 10'000);
            if (!threshold) return common::Result<void, StrategyError>::failure(threshold.error());
            imbalance_threshold_bps_ = threshold.value();
            break;
        }
        }
        return common::Result<void, StrategyError>::success();
    }

    common::Result<std::optional<StrategySignal>, StrategyError> make_signal(
        const market_data::CanonicalMarketEvent& event, const StrategyContext& context,
        const trading::Side side, const std::int64_t reference_price, std::string reason,
        std::map<std::string, std::int64_t, std::less<>> values) const {
        if (reference_price <= 0 || context.instrument.tick_size_micro <= 0 ||
            context.instrument.lot_size == 0 || context.order_quantity == 0)
            return common::Result<std::optional<StrategySignal>, StrategyError>::failure(
                {"INVALID_ORDER_SIZING", "Signal requires a positive price, tick, lot and quantity"});
        const auto lot = context.instrument.lot_size;
        auto quantity = context.order_quantity;
        if ((side == trading::Side::Buy && context.position_quantity < 0) ||
            (side == trading::Side::Sell && context.position_quantity > 0)) {
            const auto position_size = context.position_quantity < 0
                ? static_cast<common::Quantity>(-(context.position_quantity + 1)) + 1
                : static_cast<common::Quantity>(context.position_quantity);
            quantity = std::min(quantity, position_size);
        }
        quantity -= quantity % lot;
        if (context.sizing_budget_micro > 0) {
            std::int64_t adverse_price = reference_price;
            auto slippage = ceil_mul_div(reference_price, context.slippage_bps, 10'000);
            if (!slippage) return common::Result<std::optional<StrategySignal>, StrategyError>::failure(
                {slippage.error().code, slippage.error().message});
            if (side == trading::Side::Buy) {
                auto adjusted = common::checked_add(reference_price, slippage.value());
                if (!adjusted) return common::Result<std::optional<StrategySignal>, StrategyError>::failure(
                    {adjusted.error().code, adjusted.error().message});
                adverse_price = adjusted.value();
            } else {
                if (slippage.value() >= reference_price) return common::Result<std::optional<StrategySignal>, StrategyError>::success(std::nullopt);
                adverse_price -= slippage.value();
            }
            auto fee_per_unit = ceil_mul_div(adverse_price, context.fee_bps, 10'000);
            if (!fee_per_unit) return common::Result<std::optional<StrategySignal>, StrategyError>::failure(
                {fee_per_unit.error().code, fee_per_unit.error().message});
            auto unit_cost = common::checked_add(adverse_price, fee_per_unit.value());
            if (!unit_cost || unit_cost.value() <= 0) return common::Result<std::optional<StrategySignal>, StrategyError>::failure(
                {"ARITHMETIC_OVERFLOW", "Fee-aware unit cost overflow"});
            const auto affordable = static_cast<common::Quantity>(context.sizing_budget_micro / unit_cost.value());
            quantity = std::min(quantity, affordable - (affordable % lot));
        }
        if (quantity < lot || quantity > static_cast<common::Quantity>(std::numeric_limits<std::int64_t>::max()))
            return common::Result<std::optional<StrategySignal>, StrategyError>::success(std::nullopt);
        auto notional = common::checked_multiply(reference_price, static_cast<std::int64_t>(quantity));
        if (!notional) return common::Result<std::optional<StrategySignal>, StrategyError>::failure(
            {notional.error().code, notional.error().message});
        auto fee = ceil_mul_div(notional.value(), context.fee_bps, 10'000);
        if (!fee) return common::Result<std::optional<StrategySignal>, StrategyError>::failure(
            {fee.error().code, fee.error().message});
        const auto event_key = event.event_id.empty() ? std::to_string(event.event_time_ns) : event.event_id;
        const auto material = std::to_string(context.strategy_id) + ":" +
            std::to_string(context.instrument.id) + ":" + event_key + ":" + reason + ":" +
            (side == trading::Side::Buy ? "BUY" : "SELL");
        const auto signal_id = hash_string(material);
        trading::OrderIntent intent{};
        intent.client_order_id = signal_id;
        intent.account_id = context.account_id;
        intent.instrument_id = context.instrument.id;
        intent.strategy_id = context.strategy_id;
        intent.side = side;
        intent.type = trading::OrderType::Market;
        intent.quantity = quantity;
        intent.estimated_fee_micro = fee.value();
        intent.tick_size_micro = context.instrument.tick_size_micro;
        intent.lot_size = lot;
        intent.created_at_ns = event.event_time_ns;
        StrategySignal signal{signal_id, std::move(reason), std::move(intent), std::move(values)};
        return common::Result<std::optional<StrategySignal>, StrategyError>::success(std::move(signal));
    }

    common::Result<std::optional<StrategySignal>, StrategyError> on_bar(
        const market_data::CanonicalMarketEvent& event, const StrategyContext& context,
        const std::int64_t close, const std::uint64_t volume) {
        switch (kind_) {
        case BuiltinKind::SmaCrossover: {
            auto fast = fast_sma_->update(close);
            auto slow = slow_sma_->update(close);
            if (!fast) return common::Result<std::optional<StrategySignal>, StrategyError>::failure({fast.error().code, fast.error().message});
            if (!slow) return common::Result<std::optional<StrategySignal>, StrategyError>::failure({slow.error().code, slow.error().message});
            const auto current_fast = fast_sma_->value();
            const auto current_slow = slow_sma_->value();
            std::optional<StrategySignal> result;
            if (current_fast && current_slow && previous_fast_ && previous_slow_) {
                const bool crossed_up = *previous_fast_ <= *previous_slow_ && *current_fast > *current_slow;
                const bool crossed_down = *previous_fast_ >= *previous_slow_ && *current_fast < *current_slow;
                if (crossed_up && context.position_quantity <= 0) {
                    auto made = make_signal(event, context, trading::Side::Buy, close, "fast SMA crossed above slow SMA",
                                            {{"fast_sma_micro", *current_fast}, {"slow_sma_micro", *current_slow}});
                    if (!made) return made;
                    result = std::move(made.value());
                } else if (crossed_down && context.position_quantity > 0) {
                    auto made = make_signal(event, context, trading::Side::Sell, close, "fast SMA crossed below slow SMA",
                                            {{"fast_sma_micro", *current_fast}, {"slow_sma_micro", *current_slow}});
                    if (!made) return made;
                    result = std::move(made.value());
                }
            }
            if (current_fast) previous_fast_ = current_fast;
            if (current_slow) previous_slow_ = current_slow;
            return common::Result<std::optional<StrategySignal>, StrategyError>::success(std::move(result));
        }
        case BuiltinKind::RsiMeanReversion: {
            auto updated = rsi_->update(close);
            if (!updated) return common::Result<std::optional<StrategySignal>, StrategyError>::failure({updated.error().code, updated.error().message});
            const auto current = rsi_->value_bps();
            std::optional<StrategySignal> result;
            if (current && previous_rsi_) {
                std::optional<trading::Side> side;
                std::string reason;
                if (*previous_rsi_ <= oversold_bps_ && *current > oversold_bps_ && context.position_quantity <= 0) {
                    side = trading::Side::Buy;
                    reason = "RSI crossed up through oversold";
                } else if (*previous_rsi_ >= neutral_bps_ && *current < neutral_bps_ && context.position_quantity > 0) {
                    side = trading::Side::Sell;
                    reason = "RSI crossed down through neutral";
                } else if (context.allow_short && *previous_rsi_ >= overbought_bps_ &&
                           *current < overbought_bps_ && context.position_quantity >= 0) {
                    side = trading::Side::Sell;
                    reason = "RSI crossed down through overbought";
                } else if (context.allow_short && *previous_rsi_ <= neutral_bps_ &&
                           *current > neutral_bps_ && context.position_quantity < 0) {
                    side = trading::Side::Buy;
                    reason = "RSI crossed up through neutral";
                }
                if (side) {
                    auto made = make_signal(event, context, *side, close, std::move(reason), {{"rsi_bps", *current}});
                    if (!made) return made;
                    result = std::move(made.value());
                }
            }
            if (current) previous_rsi_ = current;
            return common::Result<std::optional<StrategySignal>, StrategyError>::success(std::move(result));
        }
        case BuiltinKind::Bollinger: {
            auto updated = bands_->update(close);
            if (!updated) return common::Result<std::optional<StrategySignal>, StrategyError>::failure({updated.error().code, updated.error().message});
            const auto current = bands_->value();
            std::optional<StrategySignal> result;
            if (current && previous_bands_ && previous_close_) {
                const auto width = current->upper - current->lower;
                const auto width_bps = ratio_bps(static_cast<std::uint64_t>(std::max<std::int64_t>(0, width)),
                                                 static_cast<std::uint64_t>(current->middle));
                const bool volatility_ok = width_bps && *width_bps >= minimum_width_bps_;
                std::optional<trading::Side> side;
                std::string reason;
                if (volatility_ok && bollinger_mode_ == BollingerMode::Breakout) {
                    if (close > current->upper && context.position_quantity <= 0) {
                        side = trading::Side::Buy;
                        reason = "close broke above upper Bollinger band";
                    } else if (close < current->lower &&
                               (context.position_quantity > 0 || context.allow_short)) {
                        side = trading::Side::Sell;
                        reason = "close broke below lower Bollinger band";
                    }
                } else if (bollinger_mode_ == BollingerMode::Reversion) {
                    if (*previous_close_ < previous_bands_->lower && close >= current->lower &&
                        context.position_quantity <= 0) {
                        side = trading::Side::Buy;
                        reason = "close re-entered lower Bollinger band";
                    } else if (*previous_close_ > previous_bands_->upper && close <= current->upper &&
                               (context.position_quantity > 0 || context.allow_short)) {
                        side = trading::Side::Sell;
                        reason = "close re-entered upper Bollinger band";
                    } else if (context.position_quantity > 0 && close >= current->middle) {
                        side = trading::Side::Sell;
                        reason = "price reverted to Bollinger middle";
                    } else if (context.allow_short && context.position_quantity < 0 && close <= current->middle) {
                        side = trading::Side::Buy;
                        reason = "price reverted to Bollinger middle";
                    }
                }
                if (side) {
                    auto made = make_signal(event, context, *side, close, std::move(reason),
                        {{"band_middle_micro", current->middle}, {"band_upper_micro", current->upper},
                         {"band_lower_micro", current->lower}, {"band_width_bps", width_bps.value_or(0)}});
                    if (!made) return made;
                    result = std::move(made.value());
                }
            }
            if (current) previous_bands_ = current;
            previous_close_ = close;
            return common::Result<std::optional<StrategySignal>, StrategyError>::success(std::move(result));
        }
        case BuiltinKind::VwapReversion: {
            if (context.session_key.empty() || volume == 0)
                return common::Result<std::optional<StrategySignal>, StrategyError>::success(std::nullopt);
            if (vwap_->session_key() != context.session_key) previous_close_.reset();
            auto updated = vwap_->update(close, volume, context.session_key);
            if (!updated) return common::Result<std::optional<StrategySignal>, StrategyError>::failure({updated.error().code, updated.error().message});
            const auto average = vwap_->value();
            std::optional<StrategySignal> result;
            if (average && previous_close_ && *average > 0) {
                const auto difference = close >= *average ? close - *average : *average - close;
                const auto deviation = ratio_bps(static_cast<std::uint64_t>(difference), static_cast<std::uint64_t>(*average));
                const bool moving_toward = (close < *average && close > *previous_close_) ||
                                           (close > *average && close < *previous_close_);
                std::optional<trading::Side> side;
                std::string reason;
                if (context.position_quantity == 0 && deviation && *deviation >= deviation_threshold_bps_ && moving_toward) {
                    side = close < *average ? trading::Side::Buy : trading::Side::Sell;
                    if (*side == trading::Side::Sell && !context.allow_short) side.reset();
                    reason = "price deviation is reverting toward session VWAP";
                } else if (context.position_quantity > 0 && close >= *average) {
                    side = trading::Side::Sell;
                    reason = "long position reached session VWAP";
                } else if (context.allow_short && context.position_quantity < 0 && close <= *average) {
                    side = trading::Side::Buy;
                    reason = "short position reached session VWAP";
                } else if (context.position_quantity > 0 && deviation && *deviation <= exit_buffer_bps_) {
                    side = trading::Side::Sell;
                    reason = "long position entered VWAP exit buffer";
                } else if (context.allow_short && context.position_quantity < 0 && deviation && *deviation <= exit_buffer_bps_) {
                    side = trading::Side::Buy;
                    reason = "short position entered VWAP exit buffer";
                }
                if (side) {
                    auto made = make_signal(event, context, *side, close, std::move(reason),
                        {{"session_vwap_micro", *average}, {"deviation_bps", deviation.value_or(0)}});
                    if (!made) return made;
                    result = std::move(made.value());
                }
            }
            previous_close_ = close;
            return common::Result<std::optional<StrategySignal>, StrategyError>::success(std::move(result));
        }
        case BuiltinKind::MomentumBreakout: {
            std::optional<StrategySignal> result;
            if (bars_.size() >= period_ && volumes_.size() >= volume_period_) {
                auto high = bars_.front().first;
                auto low = bars_.front().second;
                for (const auto& bar : bars_) {
                    high = std::max(high, bar.first);
                    low = std::min(low, bar.second);
                }
                std::uint64_t volume_sum = 0;
                for (const auto item : volumes_) {
                    if (item > std::numeric_limits<std::uint64_t>::max() - volume_sum)
                        return common::Result<std::optional<StrategySignal>, StrategyError>::failure(
                            {"ARITHMETIC_OVERFLOW", "Momentum volume window overflow"});
                    volume_sum += item;
                }
                const auto average_volume = volume_sum / volumes_.size();
                auto required_volume = ceil_mul_div(static_cast<std::int64_t>(average_volume), volume_factor_bps_, 10'000);
                if (!required_volume) return common::Result<std::optional<StrategySignal>, StrategyError>::failure(
                    {required_volume.error().code, required_volume.error().message});
                std::optional<trading::Side> side;
                std::string reason;
                if (volume > 0 && volume >= static_cast<std::uint64_t>(required_volume.value()) && close > high &&
                    context.position_quantity <= 0) {
                    side = trading::Side::Buy;
                    reason = "completed bar broke prior high with volume confirmation";
                } else if (volume > 0 && volume >= static_cast<std::uint64_t>(required_volume.value()) && close < low &&
                           (context.position_quantity > 0 || context.allow_short)) {
                    side = trading::Side::Sell;
                    reason = "completed bar broke prior low with volume confirmation";
                }
                if (side) {
                    auto made = make_signal(event, context, *side, close, std::move(reason),
                        {{"channel_high_micro", high}, {"channel_low_micro", low},
                         {"volume_average", static_cast<std::int64_t>(average_volume)},
                         {"bar_volume", static_cast<std::int64_t>(std::min<std::uint64_t>(volume,
                            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))}});
                    if (!made) return made;
                    result = std::move(made.value());
                }
            }
            bars_.emplace_back(event.bar->high_micro, event.bar->low_micro);
            if (bars_.size() > period_) bars_.pop_front();
            volumes_.push_back(volume);
            if (volumes_.size() > volume_period_) volumes_.pop_front();
            return common::Result<std::optional<StrategySignal>, StrategyError>::success(std::move(result));
        }
        case BuiltinKind::OrderBookImbalance: break;
        }
        return common::Result<std::optional<StrategySignal>, StrategyError>::success(std::nullopt);
    }

    common::Result<std::optional<StrategySignal>, StrategyError> on_depth(
        const market_data::CanonicalMarketEvent& event, const StrategyContext& context) {
        if (event.kind != market_data::MarketEventKind::Depth || event.bids.empty() || event.asks.empty())
            return common::Result<std::optional<StrategySignal>, StrategyError>::success(std::nullopt);
        std::uint64_t bids = 0;
        std::uint64_t asks = 0;
        for (std::size_t i = 0; i < std::min<std::size_t>(5, event.bids.size()); ++i) {
            if (event.bids[i].quantity > std::numeric_limits<std::uint64_t>::max() - bids)
                return common::Result<std::optional<StrategySignal>, StrategyError>::success(std::nullopt);
            bids += event.bids[i].quantity;
        }
        for (std::size_t i = 0; i < std::min<std::size_t>(5, event.asks.size()); ++i) {
            if (event.asks[i].quantity > std::numeric_limits<std::uint64_t>::max() - asks)
                return common::Result<std::optional<StrategySignal>, StrategyError>::success(std::nullopt);
            asks += event.asks[i].quantity;
        }
        const auto value = imbalance_->value_bps(bids, asks);
        if (!value || std::abs(*value) < imbalance_threshold_bps_)
            return common::Result<std::optional<StrategySignal>, StrategyError>::success(std::nullopt);
        std::optional<trading::Side> side;
        if (*value >= imbalance_threshold_bps_ && context.position_quantity <= 0) side = trading::Side::Buy;
        if (*value <= -imbalance_threshold_bps_ &&
            (context.position_quantity > 0 || (context.position_quantity == 0 && context.allow_short)))
            side = trading::Side::Sell;
        const auto price = side == trading::Side::Buy ? event.ask_micro : event.bid_micro;
        if (!side || !price) return common::Result<std::optional<StrategySignal>, StrategyError>::success(std::nullopt);
        return make_signal(event, context, *side, *price,
                           *value > 0 ? "top-five depth imbalance favors bids" : "top-five depth imbalance favors asks",
                           {{"imbalance_bps", *value}, {"bid_quantity", static_cast<std::int64_t>(std::min<std::uint64_t>(bids,
                                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))},
                            {"ask_quantity", static_cast<std::int64_t>(std::min<std::uint64_t>(asks,
                                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))}});
    }

    BuiltinKind kind_;
    StrategyConfig config_{};
    bool configured_{};
    bool stopped_{};
    std::size_t fast_period_{5};
    std::size_t slow_period_{20};
    std::size_t period_{14};
    std::size_t volume_period_{20};
    std::uint32_t deviation_bps_{20'000};
    std::uint32_t volume_factor_bps_{10'000};
    std::int64_t oversold_bps_{3'000};
    std::int64_t overbought_bps_{7'000};
    std::int64_t neutral_bps_{5'000};
    std::int64_t minimum_width_bps_{};
    std::int64_t deviation_threshold_bps_{100};
    std::int64_t exit_buffer_bps_{10};
    std::int64_t imbalance_threshold_bps_{1'500};
    BollingerMode bollinger_mode_{BollingerMode::Reversion};
    std::unique_ptr<SimpleMovingAverage> fast_sma_{};
    std::unique_ptr<SimpleMovingAverage> slow_sma_{};
    std::unique_ptr<RelativeStrengthIndex> rsi_{};
    std::unique_ptr<BollingerBandsIndicator> bands_{};
    std::unique_ptr<SessionVwap> vwap_{};
    std::unique_ptr<BookImbalance> imbalance_{};
    std::optional<std::int64_t> previous_fast_{};
    std::optional<std::int64_t> previous_slow_{};
    std::optional<std::int64_t> previous_rsi_{};
    std::optional<BollingerBands> previous_bands_{};
    std::optional<std::int64_t> previous_close_{};
    std::deque<std::pair<std::int64_t, std::int64_t>> bars_{};
    std::deque<std::uint64_t> volumes_{};
};

} // namespace

std::unique_ptr<IStrategy> make_builtin(const BuiltinKind kind) {
    return std::make_unique<BuiltinStrategy>(kind);
}

std::string_view to_string(const BuiltinKind kind) noexcept {
    switch (kind) {
    case BuiltinKind::SmaCrossover: return "SMA_CROSSOVER";
    case BuiltinKind::RsiMeanReversion: return "RSI_MEAN_REVERSION";
    case BuiltinKind::Bollinger: return "BOLLINGER";
    case BuiltinKind::VwapReversion: return "VWAP_REVERSION";
    case BuiltinKind::MomentumBreakout: return "MOMENTUM_BREAKOUT";
    case BuiltinKind::OrderBookImbalance: return "ORDER_BOOK_IMBALANCE";
    }
    return "UNKNOWN";
}

} // namespace basalt::strategy
