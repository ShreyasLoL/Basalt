#pragma once

#include "basalt/market_data/instrument_master.hpp"
#include "basalt/market_data/types.hpp"
#include "basalt/trading/execution.hpp"
#include "basalt/trading/risk.hpp"

#include <cstdint>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace basalt::strategy {

enum class LifecycleState : std::uint8_t {
    Created, Configured, Validated, Initialising, WarmingUp,
    Running, Paused, Stopping, Stopped, Failed
};

struct StrategyConfig {
    std::map<std::string, std::int64_t, std::less<>> integer_parameters{};
    std::map<std::string, std::string, std::less<>> text_parameters{};
    common::Quantity order_quantity{1};
    std::uint64_t random_seed{};
    std::int64_t sizing_budget_micro{};
    std::uint32_t fee_bps{};
    std::uint32_t slippage_bps{};
};

enum class StrategyParameterType : std::uint8_t { Integer, Text };

struct StrategyParameterDescriptor {
    std::string name{};
    StrategyParameterType type{StrategyParameterType::Integer};
    std::int64_t minimum{};
    std::int64_t maximum{};
    std::string default_value{};
};

struct StrategyDescriptor {
    std::string strategy_id{};
    std::string name{};
    std::string version{"1.0.0"};
    std::string stable_hash{};
    std::size_t required_warmup_events{};
    std::vector<market_data::MarketEventKind> required_channels{};
    std::vector<StrategyParameterDescriptor> parameter_schema{};
    std::vector<common::InstrumentId> supported_instruments{};
};

struct StrategyContext {
    market_data::InstrumentDefinition instrument{};
    common::AccountId account_id{};
    common::StrategyId strategy_id{};
    common::PositionQuantity position_quantity{};
    std::int64_t now_ns{};
    std::uint64_t max_data_age_ns{5'000'000'000ULL};
    common::Quantity order_quantity{1};
    std::int64_t sizing_budget_micro{};
    std::uint32_t fee_bps{};
    std::uint32_t slippage_bps{};
    std::string session_key{};
    bool allow_short{};
    trading::RiskSnapshot risk_snapshot{};
    trading::RiskLimits risk_limits{};
};

struct StrategySignal {
    std::string signal_id{};
    std::string reason{};
    trading::OrderIntent intent{};
    std::map<std::string, std::int64_t, std::less<>> values{};
};

struct StrategyError {
    std::string code{};
    std::string message{};
};

class IStrategy {
public:
    virtual ~IStrategy() = default;
    [[nodiscard]] virtual StrategyDescriptor descriptor() const = 0;
    [[nodiscard]] virtual common::Result<void, StrategyError> configure(const StrategyConfig& config) = 0;
    [[nodiscard]] virtual common::Result<void, StrategyError> initialise(const StrategyContext& context) = 0;
    [[nodiscard]] virtual common::Result<std::vector<StrategySignal>, StrategyError> on_event(
        const market_data::CanonicalMarketEvent& event, const StrategyContext& context) = 0;
    virtual void on_execution(const trading::ExecutionReport& report, const StrategyContext& context) = 0;
    virtual void on_stop() noexcept = 0;
};

struct RiskRejection {
    StrategySignal signal{};
    trading::RiskDecision decision{};
};

struct StrategyRunResult {
    std::vector<StrategySignal> approved_signals{};
    std::vector<RiskRejection> rejected_signals{};
    LifecycleState state{LifecycleState::Created};
};

class StrategyRuntime {
public:
    using FailureSink = std::function<void(common::StrategyId, const StrategyError&)>;
    StrategyRuntime(std::unique_ptr<IStrategy> strategy, trading::RiskEngine& risk,
                    FailureSink failure_sink = {});
    [[nodiscard]] LifecycleState state() const noexcept { return state_; }
    [[nodiscard]] const std::optional<StrategyError>& failure() const noexcept { return failure_; }
    [[nodiscard]] const std::vector<LifecycleState>& transitions() const noexcept { return transitions_; }
    [[nodiscard]] common::Result<void, StrategyError> configure(const StrategyConfig& config);
    [[nodiscard]] common::Result<void, StrategyError> start(const StrategyContext& context);
    [[nodiscard]] common::Result<StrategyRunResult, StrategyError> on_market_event(
        const market_data::CanonicalMarketEvent& event, const StrategyContext& context);
    [[nodiscard]] common::Result<void, StrategyError> on_execution(
        const trading::ExecutionReport& report, const StrategyContext& context);
    [[nodiscard]] common::Result<void, StrategyError> pause();
    [[nodiscard]] common::Result<void, StrategyError> resume();
    [[nodiscard]] common::Result<void, StrategyError> stop();

private:
    [[nodiscard]] common::Result<void, StrategyError> transition(LifecycleState next);
    [[nodiscard]] common::Result<void, StrategyError> fail(StrategyError error);
    [[nodiscard]] bool event_is_fresh(const market_data::CanonicalMarketEvent& event,
                                      const StrategyContext& context) const noexcept;
    std::unique_ptr<IStrategy> strategy_;
    trading::RiskEngine& risk_;
    FailureSink failure_sink_{};
    LifecycleState state_{LifecycleState::Created};
    std::optional<StrategyConfig> config_{};
    std::optional<StrategyError> failure_{};
    common::StrategyId strategy_id_{};
    std::vector<LifecycleState> transitions_{LifecycleState::Created};
    std::size_t warmup_events_seen_{};
};

[[nodiscard]] std::string_view to_string(LifecycleState state) noexcept;

} // namespace basalt::strategy
