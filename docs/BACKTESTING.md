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

## C++ implementation

`basalt_backtest` provides `BacktestEngine`, `ReplayController`, train/validation/test split validation, manifest generation and the 18-item `AnalyticsReport`. Input bars carry a stable event sequence, open and close timestamps, a canonical OHLCV event, provenance/quality and an exchange-calendar session key. A bar close timestamp is the last included nanosecond: `close = open + interval - 1`; the next bar's open must be strictly later. Bars must be unique, tick-aligned, chronologically ordered, non-overlapping and `OK`; stale, gapped, clock-skewed, malformed or out-of-range input invalidates the run.

`BacktestEngine` creates a fresh strategy runtime, kill switch, risk engine and portfolio ledger for every run. The strategy sees the completed bar close; every approved intent enters a pending-order queue and can reach `PaperExecutionSimulator` only when a later bar opens. The bar model applies configured adverse half-spread and tick slippage, participation caps and the fee schedule effective at simulated fill time. Partial fills retain their risk reservation until filled, expired or cancelled at end of data. The result includes signal and order journals, provenance-labeled executions, cash and position journals, the equity/exposure curves, risk rejection counts, warnings and hashes.

Initial cash and optional positions are recorded in the manifest; seed positions enter the ledger as zero-fee historical fills and are excluded from in-period execution and completed-round-trip counts. One instrument is simulated per run. The selected half-open split is isolated from the other ranges, and the configured embargo is validated before dispatch. VWAP runs require a session key supplied for every bar from the declared exchange calendar.

The manifest records the BASALT revision, compiler/build flags, software/schema versions, strategy ID/version/source and config hashes, dataset ID/SHA-256 plus a canonical input-event hash, instrument-master version, date/sequence range, timezone/calendar, seed, initial account state, versioned fee schedule, model/latency/slippage/participation, all risk limits, split/embargo, numeric rounding assumptions, result hashes and the survivorship/corporate-action/license declarations. Unknown survivorship safety and unspecified corporate-action/license facts produce explicit warnings. Same inputs and config produce identical input, output and manifest hashes on the same implementation.

The analytics API always returns exactly the eighteen named definitions above. A value is optional with a status such as `NO_CLOSED_TRADES`, `ZERO_DRAWDOWN_OR_UNDEFINED_CAGR`, or `INSUFFICIENT_RETURN_SAMPLES`; it is never silently replaced with zero. Auxiliary fields expose compounded relative wealth, execution count, peak exposure, fees relative to gross pre-fee P&L, signed and absolute slippage, and capacity quantity alongside their named metrics. Annualization frequency, risk-free return, downside target and benchmark return are manifest inputs. Ratios use wide display-only arithmetic after ledger values are fixed.

`ReplayController` advances a virtual clock through sequence-validated events and supports play, pause, step-one, seek and 1x/5x/10x/MAX labels. Seek restores the greatest verified checkpoint at or before the target, then applies later events through the target; the caller's restore callback validates checkpoint state/hash and the apply callback owns deterministic state replay. It does not edit source events.
