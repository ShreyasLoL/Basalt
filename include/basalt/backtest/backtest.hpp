#pragma once

#include "basalt/persistence/history.hpp"
#include "basalt/strategy/runtime.hpp"
#include "basalt/trading/ledger.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace basalt::backtest {

struct BacktestEvent {
    common::EventSequence sequence{};
    std::int64_t open_time_ns{};
    std::int64_t close_time_ns{};
    std::string session_key{};
    market_data::CanonicalMarketEvent event{};
};

struct TimeRange {
    std::int64_t start_ns{};
    std::int64_t end_ns{}; // Half-open [start,end).
};

struct DatasetSplits {
    TimeRange train{};
    TimeRange validation{};
    TimeRange test{};
    std::uint64_t embargo_ns{};
};

struct InitialPosition {
    common::InstrumentId instrument_id{};
    common::PositionQuantity quantity{};
    common::MicroUnits average_cost_micro{};
};

enum class DataSplit : std::uint8_t { Train, Validation, Test };
[[nodiscard]] common::Result<void> validate_splits(const DatasetSplits& splits);
[[nodiscard]] std::string_view to_string(DataSplit split) noexcept;

enum class ReplaySpeed : std::uint8_t { OneX, FiveX, TenX, Max };
[[nodiscard]] std::string_view to_string(ReplaySpeed speed) noexcept;

struct ReplayCheckpoint {
    common::EventSequence sequence{};
    std::int64_t time_ns{};
    std::string state_hash{};
    std::string state_blob{};
};

class ReplayController {
public:
    using Restore = std::function<common::Result<void>(const ReplayCheckpoint&)>;
    using Apply = std::function<common::Result<void>(const BacktestEvent&)>;

    explicit ReplayController(std::vector<BacktestEvent> events);
    [[nodiscard]] common::Result<void> validate() const;
    void play() noexcept { playing_ = true; }
    void pause() noexcept { playing_ = false; }
    void set_speed(ReplaySpeed speed) noexcept { speed_ = speed; }
    [[nodiscard]] ReplaySpeed speed() const noexcept { return speed_; }
    [[nodiscard]] bool playing() const noexcept { return playing_; }
    [[nodiscard]] std::optional<BacktestEvent> next();
    [[nodiscard]] common::Result<BacktestEvent> step_one();
    [[nodiscard]] common::Result<void> seek(common::EventSequence target,
                                             const std::vector<ReplayCheckpoint>& checkpoints,
                                             const Restore& restore, const Apply& apply);
    [[nodiscard]] common::EventSequence current_sequence() const noexcept { return current_sequence_; }
    [[nodiscard]] std::int64_t virtual_time_ns() const noexcept { return virtual_time_ns_; }
    [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }

private:
    std::vector<BacktestEvent> events_{};
    std::size_t cursor_{};
    common::EventSequence current_sequence_{};
    std::int64_t virtual_time_ns_{};
    bool playing_{};
    ReplaySpeed speed_{ReplaySpeed::OneX};
};

struct EquityPoint {
    std::int64_t time_ns{};
    std::int64_t equity_micro{};
};

struct ExposurePoint {
    std::int64_t time_ns{};
    std::int64_t gross_notional_micro{};
};

struct AnalyticsInput {
    std::vector<EquityPoint> equity_curve{};
    std::vector<ExposurePoint> exposure_curve{};
    std::vector<std::int64_t> closed_trade_pnl_micro{};
    std::uint64_t execution_count{};
    std::int64_t turnover_micro{};
    std::int64_t total_fees_micro{};
    std::int64_t gross_pre_fee_pnl_micro{};
    std::int64_t signed_slippage_micro{};
    std::int64_t absolute_slippage_micro{};
    std::int64_t peak_gross_exposure_micro{};
    std::int64_t estimated_capacity_notional_micro{};
    std::uint64_t estimated_capacity_quantity{};
    std::optional<double> benchmark_cumulative_return{};
    double risk_free_return_per_period{};
    double target_return_per_period{};
    std::uint64_t periods_per_year{252};
};

struct AnalyticsMetricValue {
    std::string name{};
    std::optional<double> value{};
    std::string status{"OK"};
    std::string units{};
    std::optional<std::int64_t> amount_micro{};
};

struct AnalyticsReport {
    std::vector<AnalyticsMetricValue> metrics{};
    std::optional<double> compounded_relative_wealth{};
    std::optional<double> fee_to_gross_pnl_ratio{};
    std::int64_t gross_wins_micro{};
    std::int64_t gross_losses_micro{};
    std::int64_t average_win_micro{};
    std::int64_t average_loss_micro{};
    std::uint64_t closed_round_trips{};
    std::uint64_t execution_count{};
    std::int64_t fees_micro{};
    std::int64_t signed_slippage_micro{};
    std::int64_t absolute_slippage_micro{};
    std::int64_t peak_gross_exposure_micro{};
    std::uint64_t estimated_capacity_quantity{};
};

[[nodiscard]] AnalyticsReport calculate_metrics(const AnalyticsInput& input);

struct BacktestConfig {
    std::string basalt_revision{"unknown"};
    std::uint32_t schema_version{1};
    std::string dataset_id{};
    std::string dataset_sha256{};
    std::string data_source{};
    std::string license_tag{};
    std::string corporate_action_policy{"unspecified"};
    bool survivorship_safe_history{};
    std::uint32_t instrument_master_version{};
    market_data::InstrumentDefinition instrument{};
    common::AccountId account_id{};
    common::StrategyId strategy_id{};
    std::int64_t initial_cash_micro{};
    std::vector<InitialPosition> initial_positions{};
    bool allow_short{};
    std::string timezone{"Asia/Kolkata"};
    std::string exchange_calendar{};
    std::uint64_t seed{};
    trading::ExecutionConfig execution{};
    common::MicroUnits half_spread_micro{};
    trading::RiskLimits risk_limits{};
    DatasetSplits splits{};
    DataSplit selected_split{DataSplit::Test};
    std::uint64_t max_working_bars{5};
    std::uint64_t bar_interval_ns{};
    std::string rounding_policy{"integer division toward zero; fee lines round upward"};
    std::string compiler{"unknown"};
    std::string build_flags{"C++20 Release"};
    std::string software_version{"1.0.0"};
    std::optional<double> benchmark_cumulative_return{};
    double risk_free_return_per_period{};
    double target_return_per_period{};
    std::uint64_t periods_per_year{252};
};

struct BacktestResult {
    bool valid{};
    std::string invalid_reason{};
    std::string manifest_json{};
    std::string manifest_sha256{};
    std::string input_event_sha256{};
    std::string output_sha256{};
    std::vector<trading::ExecutionFill> fills{};
    std::vector<trading::CashJournalEntry> cash_journal{};
    std::vector<EquityPoint> equity_curve{};
    std::vector<ExposurePoint> exposure_curve{};
    struct SignalRecord {
        common::EventSequence event_sequence{};
        std::int64_t event_time_ns{};
        std::string signal_id{};
        std::string reason{};
        trading::Side side{trading::Side::Buy};
        common::Quantity quantity{};
        common::MicroUnits reference_price_micro{};
        bool approved{};
        std::string risk_code{};
    };
    struct OrderRecord {
        std::uint64_t order_id{};
        std::string client_order_id{};
        common::EventSequence signal_sequence{};
        std::int64_t created_at_ns{};
        std::int64_t updated_at_ns{};
        trading::Side side{trading::Side::Buy};
        common::Quantity requested_quantity{};
        common::Quantity filled_quantity{};
        common::Quantity remaining_quantity{};
        std::string status{};
        std::string reason{};
    };
    struct PositionRecord {
        common::EventSequence event_sequence{};
        std::int64_t time_ns{};
        common::PositionQuantity quantity{};
        common::MicroUnits average_cost_micro{};
        std::int64_t realized_pnl_micro{};
        std::int64_t unrealized_pnl_micro{};
        std::optional<common::MicroUnits> mark_micro{};
        std::string mark_source{};
    };
    std::vector<SignalRecord> signals{};
    std::vector<OrderRecord> orders{};
    std::vector<PositionRecord> position_journal{};
    std::vector<std::int64_t> closed_trade_pnl_micro{};
    std::vector<std::pair<std::string, std::uint64_t>> risk_rejections{};
    std::vector<std::string> warnings{};
    std::uint64_t execution_count{};
    std::uint64_t completed_orders{};
    std::uint64_t cancelled_orders{};
    std::uint64_t rejected_orders{};
    std::int64_t turnover_micro{};
    std::int64_t total_fees_micro{};
    std::int64_t gross_pre_fee_pnl_micro{};
    std::int64_t signed_slippage_micro{};
    std::int64_t absolute_slippage_micro{};
    std::int64_t peak_gross_exposure_micro{};
    std::int64_t estimated_capacity_notional_micro{};
    std::uint64_t estimated_capacity_quantity{};
    AnalyticsReport analytics{};
};

class BacktestEngine {
public:
    [[nodiscard]] common::Result<BacktestResult> run(
        std::unique_ptr<strategy::IStrategy> strategy,
        const strategy::StrategyConfig& strategy_config,
        const std::vector<BacktestEvent>& events,
        const BacktestConfig& config) const;
};

} // namespace basalt::backtest
