#include "basalt/backtest/backtest.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace basalt;

class BuyOnceStrategy final : public strategy::IStrategy {
public:
    strategy::StrategyDescriptor descriptor() const override {
        strategy::StrategyDescriptor value{};
        value.strategy_id = "PORTFOLIO_DEMO_BUY_ONCE";
        value.name = "Portfolio demo buy once";
        value.stable_hash = "portfolio-demo-buy-once-v1";
        value.required_channels = {market_data::MarketEventKind::Bar};
        return value;
    }

    common::Result<void, strategy::StrategyError> configure(const strategy::StrategyConfig&) override {
        configured_ = true;
        return common::Result<void, strategy::StrategyError>::success();
    }

    common::Result<void, strategy::StrategyError> initialise(const strategy::StrategyContext& context) override {
        if (!configured_ || context.instrument.id == 0)
            return common::Result<void, strategy::StrategyError>::failure(
                {"DEMO_CONTEXT", "A configured strategy and instrument are required"});
        return common::Result<void, strategy::StrategyError>::success();
    }

    common::Result<std::vector<strategy::StrategySignal>, strategy::StrategyError> on_event(
        const market_data::CanonicalMarketEvent&, const strategy::StrategyContext& context) override {
        if (sent_) return common::Result<std::vector<strategy::StrategySignal>, strategy::StrategyError>::success({});
        sent_ = true;
        strategy::StrategySignal signal{};
        signal.signal_id = "portfolio-demo-" + std::to_string(context.instrument.id);
        signal.reason = "one deterministic buy per instrument";
        signal.intent.client_order_id = signal.signal_id;
        signal.intent.account_id = context.account_id;
        signal.intent.instrument_id = context.instrument.id;
        signal.intent.strategy_id = context.strategy_id;
        signal.intent.side = trading::Side::Buy;
        signal.intent.type = trading::OrderType::Market;
        signal.intent.quantity = context.order_quantity;
        signal.intent.tick_size_micro = context.instrument.tick_size_micro;
        signal.intent.lot_size = context.instrument.lot_size;
        signal.intent.created_at_ns = context.now_ns;
        return common::Result<std::vector<strategy::StrategySignal>, strategy::StrategyError>::success({signal});
    }

    void on_execution(const trading::ExecutionReport&, const strategy::StrategyContext&) override {}
    void on_stop() noexcept override {}

private:
    bool configured_{};
    bool sent_{};
};

backtest::BacktestEvent make_bar(const common::InstrumentId id, const std::uint64_t sequence,
                                const std::int64_t open_time, const std::int64_t price) {
    backtest::BacktestEvent record{};
    record.sequence = sequence;
    record.open_time_ns = open_time;
    record.close_time_ns = open_time + 99;
    record.session_key = "SIM-SESSION";
    record.event.event_id = "portfolio-demo-" + std::to_string(id) + "-" + std::to_string(sequence);
    record.event.instrument_id = id;
    record.event.kind = market_data::MarketEventKind::Bar;
    record.event.event_time_ns = record.close_time_ns;
    record.event.receive_time_utc_ns = record.close_time_ns;
    record.event.source = common::DataSourceType::Historical;
    record.event.quality = common::FeedQualityState::Ok;
    record.event.bar = market_data::OhlcvBar{price, price, price, price, 100};
    return record;
}

backtest::PortfolioBacktestLeg make_leg(const common::InstrumentId id, const common::StrategyId strategy_id,
                                       const std::string& symbol) {
    backtest::BacktestConfig config{};
    config.dataset_id = "portfolio-demo-bars-" + symbol;
    config.dataset_sha256 = std::string(64, 'a');
    config.data_source = "deterministic synthetic example";
    config.license_tag = "SYNTHETIC";
    config.corporate_action_policy = "not applicable to synthetic data";
    config.instrument_master_version = 1;
    config.instrument.id = id;
    config.instrument.symbol = symbol;
    config.instrument.display_symbol = symbol;
    config.instrument.segment = "SIM";
    config.instrument.tick_size_micro = 1;
    config.instrument.lot_size = 1;
    config.account_id = 1;
    config.strategy_id = strategy_id;
    config.initial_cash_micro = 150;
    config.exchange_calendar = "SIM-24x7";
    config.seed = 42;
    config.execution.model = trading::PaperExecutionModel::BarApproximation;
    config.execution.tick_size_micro = 1;
    config.execution.participation_bps = 10'000;
    config.execution.slippage_ticks = 0;
    config.half_spread_micro = 0;
    config.splits = {{1, 100}, {110, 200}, {210, 2'000}, 0};
    config.selected_split = backtest::DataSplit::Test;
    config.bar_interval_ns = 100;
    strategy::StrategyConfig strategy_config{};
    std::vector<backtest::BacktestEvent> events{
        make_bar(id, 1, 1'000, 100), make_bar(id, 2, 1'100, 100)};
    return {std::move(config), std::move(strategy_config), std::move(events),
            std::make_unique<BuyOnceStrategy>()};
}

} // namespace

int main() {
    backtest::PortfolioBacktestConfig config{};
    config.account_id = 1;
    config.initial_cash_micro = 150;
    config.risk_limits.max_gross_exposure_micro = 1'000;
    config.risk_limits.max_net_exposure_micro = 1'000;
    std::vector<backtest::PortfolioBacktestLeg> legs;
    legs.push_back(make_leg(7, 101, "SIM-A"));
    legs.push_back(make_leg(8, 102, "SIM-B"));

    backtest::PortfolioBacktestEngine engine;
    auto result = engine.run(std::move(legs), config);
    if (!result) {
        std::cerr << result.error().code << ": " << result.error().message << '\n';
        return 1;
    }
    std::cout << "BASALT shared-capital portfolio backtest\n"
              << "instruments: " << result.value().instruments.size() << '\n'
              << "fills: " << result.value().fills.size() << '\n'
              << "cash (micro-INR): " << result.value().final_portfolio.cash_balance_micro << '\n'
              << "combined ledger SHA-256: " << result.value().ledger_sha256 << '\n';
    for (const auto& warning : result.value().warnings) std::cout << "warning: " << warning << '\n';
    for (const auto& [code, count] : result.value().risk_rejections)
        std::cout << "risk rejection: " << code << " x " << count << '\n';
    return 0;
}
