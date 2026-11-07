#include "basalt/backtest/backtest.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace basalt::backtest {
namespace {

using Wide = long double;

double as_double(const Wide value) noexcept { return static_cast<double>(value); }

void add(AnalyticsReport& report, std::string name, std::optional<double> value,
         std::string status, std::string units, std::optional<std::int64_t> amount = std::nullopt) {
    if (value && !std::isfinite(*value)) {
        value.reset();
        status = "UNDEFINED_NONFINITE";
    }
    report.metrics.push_back({std::move(name), value, std::move(status), std::move(units), amount});
}

std::optional<double> ratio(const Wide numerator, const Wide denominator) noexcept {
    if (denominator == 0) return std::nullopt;
    return as_double(numerator / denominator);
}

} // namespace

AnalyticsReport calculate_metrics(const AnalyticsInput& input) {
    AnalyticsReport report{};
    report.execution_count = input.execution_count;
    report.fees_micro = input.total_fees_micro;
    report.signed_slippage_micro = input.signed_slippage_micro;
    report.absolute_slippage_micro = input.absolute_slippage_micro;
    report.peak_gross_exposure_micro = input.peak_gross_exposure_micro;
    report.estimated_capacity_quantity = input.estimated_capacity_quantity;
    const auto enough_equity = input.equity_curve.size() >= 2;
    std::vector<Wide> returns;
    bool equity_valid = enough_equity;
    for (std::size_t i = 0; i < input.equity_curve.size(); ++i) {
        const auto& point = input.equity_curve[i];
        if (point.time_ns <= 0 || point.equity_micro <= 0 ||
            (i > 0 && point.time_ns <= input.equity_curve[i - 1].time_ns)) equity_valid = false;
        if (i > 0) {
            const auto previous = input.equity_curve[i - 1].equity_micro;
            if (previous <= 0) equity_valid = false;
            else returns.push_back(static_cast<Wide>(point.equity_micro) / previous - 1.0L);
        }
    }
    const auto initial_equity = input.equity_curve.empty() ? 0 : input.equity_curve.front().equity_micro;
    const auto final_equity = input.equity_curve.empty() ? 0 : input.equity_curve.back().equity_micro;
    std::optional<Wide> total_return;
    if (equity_valid && initial_equity > 0)
        total_return = static_cast<Wide>(final_equity) / initial_equity - 1.0L;
    add(report, "Total Return", total_return ? std::optional<double>{as_double(*total_return)} : std::nullopt,
        total_return ? "OK" : "INSUFFICIENT_OR_INVALID_EQUITY", "fraction");

    std::optional<Wide> relative;
    std::optional<Wide> relative_wealth;
    if (total_return && input.benchmark_cumulative_return && *input.benchmark_cumulative_return > -1.0) {
        relative = *total_return - *input.benchmark_cumulative_return;
        relative_wealth = (1.0L + *total_return) / (1.0L + *input.benchmark_cumulative_return) - 1.0L;
    }
    add(report, "Benchmark-Relative Return", relative ? std::optional<double>{as_double(*relative)} : std::nullopt,
        relative ? "OK" : "BENCHMARK_UNAVAILABLE", "fraction");
    if (relative_wealth) report.compounded_relative_wealth = as_double(*relative_wealth);

    std::optional<Wide> cagr;
    if (total_return && final_equity > 0) {
        const auto elapsed_ns = input.equity_curve.back().time_ns - input.equity_curve.front().time_ns;
        if (elapsed_ns > 0) {
            const auto elapsed_seconds = static_cast<Wide>(elapsed_ns) / 1'000'000'000.0L;
            cagr = std::pow(static_cast<Wide>(final_equity) / initial_equity,
                            31'557'600.0L / elapsed_seconds) - 1.0L;
        }
    }
    add(report, "CAGR", cagr ? std::optional<double>{as_double(*cagr)} : std::nullopt,
        cagr ? "OK" : "NONPOSITIVE_EQUITY_OR_ZERO_DURATION", "annualized_fraction");

    std::optional<Wide> mean_return;
    Wide sum_return = 0;
    for (const auto value : returns) sum_return += value;
    if (!returns.empty()) mean_return = sum_return / returns.size();
    std::optional<Wide> sample_variance;
    if (returns.size() >= 2 && mean_return) {
        Wide sum_squares = 0;
        for (const auto value : returns) sum_squares += (value - *mean_return) * (value - *mean_return);
        sample_variance = sum_squares / static_cast<Wide>(returns.size() - 1);
    }
    std::optional<Wide> volatility;
    if (sample_variance && input.periods_per_year > 0)
        volatility = std::sqrt(*sample_variance * input.periods_per_year);
    add(report, "Volatility", volatility ? std::optional<double>{as_double(*volatility)} : std::nullopt,
        volatility ? "OK" : "INSUFFICIENT_RETURN_SAMPLES", "annualized_fraction");

    const auto risk_free = static_cast<Wide>(input.risk_free_return_per_period);
    Wide sum_excess = 0;
    for (const auto value : returns) sum_excess += value - risk_free;
    std::optional<Wide> mean_excess;
    if (!returns.empty()) mean_excess = sum_excess / returns.size();
    std::optional<Wide> sharpe;
    if (mean_excess && sample_variance && *sample_variance > 0 && input.periods_per_year > 0)
        sharpe = *mean_excess / std::sqrt(*sample_variance) * std::sqrt(static_cast<Wide>(input.periods_per_year));
    add(report, "Sharpe", sharpe ? std::optional<double>{as_double(*sharpe)} : std::nullopt,
        sharpe ? "OK" : (sample_variance && *sample_variance == 0 ? "ZERO_RETURN_VARIANCE" : "INSUFFICIENT_RETURN_SAMPLES"),
        "annualized_ratio");

    Wide downside_sum = 0;
    for (const auto value : returns) {
        const auto downside = std::min<Wide>(0, value - static_cast<Wide>(input.target_return_per_period));
        downside_sum += downside * downside;
    }
    std::optional<Wide> downside_deviation;
    if (!returns.empty()) downside_deviation = std::sqrt(downside_sum / returns.size());
    std::optional<Wide> sortino;
    if (mean_excess && downside_deviation && *downside_deviation > 0 && input.periods_per_year > 0)
        sortino = *mean_excess / *downside_deviation * std::sqrt(static_cast<Wide>(input.periods_per_year));
    add(report, "Sortino", sortino ? std::optional<double>{as_double(*sortino)} : std::nullopt,
        sortino ? "OK" : (downside_deviation && *downside_deviation == 0 ? "ZERO_DOWNSIDE_DEVIATION" : "INSUFFICIENT_RETURN_SAMPLES"),
        "annualized_ratio");

    std::optional<Wide> maximum_drawdown;
    if (equity_valid) {
        std::int64_t peak = input.equity_curve.front().equity_micro;
        Wide max_drawdown = 0;
        for (const auto& point : input.equity_curve) {
            peak = std::max(peak, point.equity_micro);
            max_drawdown = std::max(max_drawdown,
                (static_cast<Wide>(peak) - point.equity_micro) / peak);
        }
        maximum_drawdown = max_drawdown;
    }
    add(report, "Maximum Drawdown", maximum_drawdown ? std::optional<double>{as_double(*maximum_drawdown)} : std::nullopt,
        maximum_drawdown ? "OK" : "INSUFFICIENT_OR_INVALID_EQUITY", "fraction");
    std::optional<Wide> calmar;
    if (cagr && maximum_drawdown && *maximum_drawdown > 0) calmar = *cagr / *maximum_drawdown;
    add(report, "Calmar", calmar ? std::optional<double>{as_double(*calmar)} : std::nullopt,
        calmar ? "OK" : "ZERO_DRAWDOWN_OR_UNDEFINED_CAGR", "ratio");

    Wide wins = 0;
    Wide losses = 0;
    std::uint64_t win_count = 0;
    std::uint64_t loss_count = 0;
    for (const auto pnl : input.closed_trade_pnl_micro) {
        if (pnl > 0) {
            wins += pnl;
            ++win_count;
        } else if (pnl < 0) {
            losses += -static_cast<Wide>(pnl);
            ++loss_count;
        }
    }
    const auto closed = static_cast<std::uint64_t>(input.closed_trade_pnl_micro.size());
    report.closed_round_trips = closed;
    const auto max_amount = static_cast<Wide>(std::numeric_limits<std::int64_t>::max());
    report.gross_wins_micro = static_cast<std::int64_t>(std::min(wins, max_amount));
    report.gross_losses_micro = static_cast<std::int64_t>(std::min(losses, max_amount));
    if (win_count > 0) report.average_win_micro = static_cast<std::int64_t>(std::min(wins / win_count, max_amount));
    if (loss_count > 0) report.average_loss_micro = static_cast<std::int64_t>(std::min(losses / loss_count, max_amount));
    add(report, "Win Rate", closed > 0 ? std::optional<double>{static_cast<double>(win_count) / closed} : std::nullopt,
        closed > 0 ? "OK" : "NO_CLOSED_TRADES", "fraction");
    const auto profit_factor = loss_count > 0 ? ratio(wins, losses) : std::nullopt;
    add(report, "Profit Factor", profit_factor,
        profit_factor ? "OK" : "NO_GROSS_LOSSES", "ratio");
    const auto avg_win = win_count > 0 ? wins / win_count : 0;
    const auto avg_loss = loss_count > 0 ? losses / loss_count : 0;
    const auto average_win_loss =
        (win_count > 0 && loss_count > 0) ? ratio(avg_win, avg_loss) : std::nullopt;
    const std::string average_win_loss_status =
        average_win_loss ? "OK" : (loss_count == 0 ? "NO_AVERAGE_LOSS" : "NO_WINS");
    add(report, "Average Win/Loss", average_win_loss, average_win_loss_status, "ratio");
    std::optional<double> expectancy;
    if (closed > 0) {
        const auto win_probability = static_cast<Wide>(win_count) / closed;
        const auto loss_probability = static_cast<Wide>(loss_count) / closed;
        expectancy = as_double(win_probability * avg_win - loss_probability * avg_loss);
    }
    add(report, "Expectancy", expectancy, expectancy ? "OK" : "NO_CLOSED_TRADES",
        "micro_INR_per_closed_trade", closed > 0 ? std::optional<std::int64_t>{static_cast<std::int64_t>(
            std::accumulate(input.closed_trade_pnl_micro.begin(), input.closed_trade_pnl_micro.end(), Wide{0}) / closed)} : std::nullopt);

    Wide average_equity = 0;
    if (!input.equity_curve.empty()) {
        for (const auto& point : input.equity_curve) average_equity += point.equity_micro;
        average_equity /= input.equity_curve.size();
    }
    const auto turnover = input.turnover_micro >= 0 && average_equity > 0
        ? ratio(input.turnover_micro, average_equity) : std::nullopt;
    add(report, "Turnover", turnover, turnover ? "OK" : "NO_POSITIVE_AVERAGE_EQUITY", "equity_turnovers");

    std::optional<double> exposure;
    if (input.exposure_curve.size() >= 2 && average_equity > 0) {
        Wide exposure_time = 0;
        bool valid = true;
        for (std::size_t i = 1; i < input.exposure_curve.size(); ++i) {
            const auto& left = input.exposure_curve[i - 1];
            const auto& right = input.exposure_curve[i];
            if (right.time_ns <= left.time_ns || left.gross_notional_micro < 0 || right.gross_notional_micro < 0) {
                valid = false;
                break;
            }
            const auto duration = static_cast<Wide>(right.time_ns - left.time_ns);
            exposure_time += (static_cast<Wide>(left.gross_notional_micro) + right.gross_notional_micro) * 0.5L * duration;
        }
        const auto duration = static_cast<Wide>(input.exposure_curve.back().time_ns - input.exposure_curve.front().time_ns);
        if (valid && duration > 0) exposure = as_double(exposure_time / duration / average_equity);
    }
    add(report, "Exposure", exposure, exposure ? "OK" : "INSUFFICIENT_EXPOSURE_CURVE", "average_gross_exposure_over_average_equity");
    add(report, "Trade Count", static_cast<double>(closed), "OK", "completed_round_trips");

    std::optional<double> fee_impact;
    if (initial_equity > 0 && input.total_fees_micro >= 0)
        fee_impact = static_cast<double>(input.total_fees_micro) / static_cast<double>(initial_equity);
    add(report, "Fee Impact", fee_impact, fee_impact ? "OK" : "INVALID_INITIAL_EQUITY", "fees_over_initial_equity",
        input.total_fees_micro);
    if (input.gross_pre_fee_pnl_micro != 0)
        report.fee_to_gross_pnl_ratio = static_cast<double>(input.total_fees_micro) /
                                        std::abs(static_cast<double>(input.gross_pre_fee_pnl_micro));

    add(report, "Slippage Impact", static_cast<double>(input.signed_slippage_micro), "OK", "signed_micro_INR",
        input.signed_slippage_micro);

    add(report, "Capacity", static_cast<double>(input.estimated_capacity_notional_micro), "MODEL_ESTIMATE",
        "estimated_micro_INR", input.estimated_capacity_notional_micro);
    return report;
}

} // namespace basalt::backtest
