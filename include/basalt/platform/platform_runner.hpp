#pragma once

#include "basalt/market_data/event_bus.hpp"
#include "basalt/market_data/pipeline.hpp"
#include "basalt/market_data/providers.hpp"
#include "basalt/strategy/runtime.hpp"
#include "basalt/trading/execution.hpp"
#include "basalt/trading/ledger.hpp"
#include "basalt/trading/risk.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace basalt::platform {

struct PlatformRunnerConfig {
    std::uint64_t random_seed{42};
    std::int64_t base_event_time_ns{1'800'000'000'000'000'000LL};
    common::InstrumentId instrument_id{9'001};
    common::AccountId account_id{1};
    common::StrategyId strategy_id{1};
    std::int64_t initial_cash_micro{1'000'000'000'000LL};
    std::uint64_t max_data_age_ns{5'000'000'000ULL};
    std::size_t event_queue_capacity{64};
    trading::RiskLimits risk_limits = [] {
        trading::RiskLimits limits{};
        limits.max_order_quantity = 1;
        limits.max_position_quantity = 100;
        limits.max_instrument_position_quantity = 100;
        return limits;
    }();
    trading::ExecutionConfig execution_config{};
    strategy::StrategyConfig strategy_config{};
};

struct PlatformSessionSnapshot {
    std::size_t ticks_processed{};
    std::size_t signals{};
    std::size_t orders{};
    std::size_t fills{};
    std::size_t risk_rejections{};
    std::vector<std::string> risk_rejection_codes{};
    std::vector<std::pair<std::string, std::uint64_t>> risk_rejection_events{};
    std::int64_t total_pnl_micro{};
    std::uint64_t ledger_hash{};
    bool execution_halted{};
    trading::PortfolioSummary portfolio{};
    trading::RiskLimits risk_limits{};
};

struct PlatformMarketSnapshot {
    market_data::InstrumentDefinition instrument{};
    std::optional<market_data::CanonicalMarketEvent> event{};
};

struct PlatformTelemetrySnapshot {
    market_data::PipelineStats pipeline{};
    market_data::EventBusStats event_bus{};
};

// A synchronous, deterministic application boundary that connects the offline
// provider through market-data validation, strategy, risk, paper execution and
// the ledger. Adapters can read the resulting portfolio snapshot without owning
// any of those domain components.
class PlatformRunner {
public:
    explicit PlatformRunner(PlatformRunnerConfig config = {},
                            std::unique_ptr<strategy::IStrategy> strategy = {});
    PlatformRunner(const PlatformRunner&) = delete;
    PlatformRunner& operator=(const PlatformRunner&) = delete;

    [[nodiscard]] common::Result<PlatformSessionSnapshot> run(std::size_t cycles);
    [[nodiscard]] common::Result<PlatformSessionSnapshot> snapshot() const;
    [[nodiscard]] common::Result<trading::PortfolioSummary> portfolio(
        common::AccountId account_id) const;
    [[nodiscard]] std::vector<PlatformMarketSnapshot> market_watch() const;
    [[nodiscard]] common::Result<PlatformMarketSnapshot> market_snapshot(
        common::InstrumentId instrument_id) const;
    [[nodiscard]] std::vector<trading::ExecutionFill> recent_fills(std::size_t limit) const;
    [[nodiscard]] PlatformTelemetrySnapshot telemetry() const;
    [[nodiscard]] common::Result<void> verify_reconciliation() const;
    [[nodiscard]] trading::KillSwitchEvent activate_kill_switch(
        std::string actor, std::string reason);

private:
    PlatformRunnerConfig config_;
    market_data::InstrumentMaster instruments_;
    const market_data::InstrumentDefinition* instrument_{};
    market_data::DeterministicSyntheticProvider provider_;
    market_data::MarketDataPipeline pipeline_;
    market_data::BoundedMarketEventBus event_bus_;
    trading::KillSwitch kill_switch_;
    trading::RiskEngine risk_engine_;
    trading::PaperExecutionSimulator execution_simulator_;
    trading::PortfolioLedger ledger_;
    strategy::StrategyRuntime strategy_runtime_;
    mutable std::mutex mutex_{};
    bool initialized_{};
    bool halted_{};
    std::int64_t next_receive_mono_ns_{1};
    std::uint64_t next_execution_id_{1};
    std::uint64_t next_ledger_sequence_{1};
    std::uint64_t next_order_id_{1};
    std::int64_t high_water_mark_micro_{};
    std::size_t ticks_processed_{};
    std::size_t signals_{};
    std::size_t orders_{};
    std::size_t fills_{};
    std::vector<std::string> risk_rejection_codes_{};
    std::vector<std::pair<std::string, std::uint64_t>> risk_rejections_{};
    std::optional<common::Error> callback_error_{};
    std::optional<market_data::CanonicalMarketEvent> latest_market_event_{};

    [[nodiscard]] common::Result<void> initialize();
    [[nodiscard]] common::Result<void> process_event(
        const market_data::CanonicalMarketEvent& event);
    [[nodiscard]] common::Result<trading::RiskSnapshot> make_risk_snapshot(
        const market_data::CanonicalMarketEvent& event);
    [[nodiscard]] common::Result<void> execute_signal(
        const strategy::StrategySignal& signal,
        const market_data::CanonicalMarketEvent& event,
        const strategy::StrategyContext& context);
    [[nodiscard]] common::Result<PlatformSessionSnapshot> snapshot_locked() const;
};

} // namespace basalt::platform
