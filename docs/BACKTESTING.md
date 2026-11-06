# Backtesting, Replay and Analytics

## Causal backtest rules

Input bars/ticks are immutable chronological data with dataset hash, source, quality, timezone, corporate-action adjustment policy and license metadata. Events are sorted by event time then stable event sequence; duplicate or out-of-order rows are rejected or quarantined with a report. A strategy observes only data available at the event boundary. A signal generated from bar t close cannot execute before bar t+1 open. Quote/depth replay uses only the state observable at the configured simulated arrival time. No current-bar high/low may decide an order generated from that bar's close.

Splits support explicit train, validation and held-out test ranges by date or event sequence, with configurable embargo/gap. Fit thresholds only on training data; choose models/parameters only on validation; report final test once. Point-in-time instrument membership, delisted symbols and corporate actions are represented when data is available; missing survivorship-safe history is prominently disclosed.

Execution is selected by named PaperExecutionModel and its exact fee/slippage/latency parameters are part of the run manifest. Outputs include signal/order/execution/cash/position journals, equity curve, drawdown curve, rejection counts, missing/stale data warnings and deterministic result hashes. A run is invalid when required input fails quality gates.

## Reproducibility manifest

Record BASALT revision, schema version, strategy ID/version/source hash, serialized config hash, dataset IDs and SHA-256, instrument-master version, input event range, timezone/calendar, seed, initial cash/positions, fee schedule version, slippage and execution model, risk config, split/embargo, numeric rounding policy, compiler/build flags, software versions and output hashes. Same manifest must replay to identical event order, executions, ledger state and metrics.

## Replay controls

ReplayEngine reads validated .blog or CSV/JSON events and advances a virtual clock. It supports Play, Pause, StepOne, Seek, and speed multipliers 1x/5x/10x/MAX. Seeking loads the nearest verified checkpoint and replays through the target sequence; it never mutates historical source. Repeated runs start from a fresh deterministic state. UI pause/seek does not affect persisted source order.

## Eighteen analytics definitions

Let E0 be initial equity, Et equity at time t, r_t=Et/Et-1-1, rf the selected annual risk-free rate, and benchmark return rb over the same range. Document annualization frequency N and currency for every report.

1. **Total Return:** Efinal/E0 - 1.
2. **Benchmark-Relative Return:** strategy cumulative return minus benchmark cumulative return; also report compounded relative wealth separately.
3. **CAGR:** (Efinal/E0)^(seconds_per_year/elapsed_seconds)-1 for positive equity and duration.
4. **Volatility:** sample standard deviation of periodic returns × sqrt(N).
5. **Sharpe:** mean(r-rf)/sample_std(r-rf) × sqrt(N); undefined when denominator is zero.
6. **Sortino:** mean(r-rf)/downside deviation × sqrt(N), with target return explicit.
7. **Maximum Drawdown:** max over t of (running_peak_t-Et)/running_peak_t.
8. **Calmar:** CAGR/maximum drawdown; undefined at zero drawdown.
9. **Win Rate:** profitable closed round trips / all closed round trips; open positions excluded.
10. **Profit Factor:** gross realized wins / absolute gross realized losses; undefined without losses.
11. **Average Win/Loss:** mean positive realized trade P&L / absolute mean negative realized trade P&L.
12. **Expectancy:** win_probability×average_win - loss_probability×average_loss; units are currency per closed trade.
13. **Turnover:** sum absolute traded notional / average equity for the period.
14. **Exposure:** time-weighted gross notional / equity, reported alongside peak exposure.
15. **Trade Count:** completed round trips and execution count, shown separately.
16. **Fee Impact:** fees divided by initial equity and by gross pre-fee P&L, with both amounts shown.
17. **Slippage Impact:** simulated execution price versus declared reference price, accumulated as signed and absolute fixed-point amounts.
18. **Capacity:** estimated max notional/quantity under configured participation, liquidity and risk caps; label as model output, never observed market capacity.

No metric is coerced to zero when undefined; return null plus an explanatory status. Analytics ratios are derived display values and may use floating-point computation only after authoritative fixed-point ledger values are finalized.
