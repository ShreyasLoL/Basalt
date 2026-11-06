# Paper Trading and Execution Models

## Safety and liquidity separation

Provider quotes/depth are observations of external liquidity. They never become resting orders in BASALT's internal CLOB. The internal book contains only BASALT simulated participant orders. A strategy/account selects one named paper model; external observations can produce simulated fill reports only through that model. No live broker order entry or credential-bearing execution adapter exists. Every order, fill, mark and metric is labeled SIMULATED and stores its execution model and source provenance.

## Four paper execution models

1. **Conservative Quote-Based (default for provider observations):** Buy market fills reference ask plus configured adverse slippage ticks; sell market fills reference bid minus slippage. Never fill optimistically at LTP. Limit buy is eligible only when observed ask is at or below limit; limit sell only when bid is at or above limit. Quantity is capped by visible top-of-book quantity and participation setting. Missing/stale quote rejects or remains unfilled according to explicit policy.
2. **Bar-Based Approximation:** Signal from completed bar t executes no earlier than next bar open. Market execution uses next open plus/minus half-spread and adverse slippage. Limit fills require next-bar price path to strictly breach the limit and are volume capped; if OHLC cannot establish intrabar ordering, choose the pessimistic path. Default participation cap is configurable (seed value 10% is a starting parameter, not a recommendation).
3. **Order-Book Replay:** Sweep recorded depth levels at simulated arrival time plus configurable latency. Consume only recorded available quantity, calculate notional/VWAP using fixed-point integers, and model queue position only when event data supports it. Missing depth or sequence gaps prevent fills.
4. **Internal CLOB:** Route into BASALT's MatchingEngine/MultiInstrumentEngine. User, strategy and explicitly named synthetic participant orders share the internal simulated book under price-time priority. Provider quote events do not populate this book.

Each model returns fill reports with requested/filled/remainder quantities, reference data event sequence, source, quality, model version, latency, slippage, fee schedule and reason. Partial fills and cancellation are first-class. The selected model is stored in the order and run manifest.

## Fees and slippage

FeeSchedule is immutable/versioned and instrument/venue/product specific. Fields include brokerage (fixed or basis points, with explicit cap), STT, exchange transaction charge, SEBI turnover charge, stamp duty, GST base/rate, and other disclosed statutory fees. Compute from fixed-point notional with checked wide intermediates and deterministic per-line rounding. Rules may vary by buy/sell, segment, delivery/intraday, turnover bracket and date. Do not ship seed fee figures as current facts: the user selects/imports an official schedule, and the UI displays schedule version/source. Slippage is configured in ticks or explicit price impact model, charged adversely and reported separately from fees.

## Paper account controls

Accounts start with an explicit cash and position seed. Orders are not silently funded by provider balances. Open order reservations, cash/position changes, fees, executions and P&L are journaled. Reports state that results are simulated and do not represent broker fills or achievable returns. Reset creates a new account/run ID or explicit reset event; it does not erase audit history.

The current C++ API is in `include/basalt/trading/`. It provides the four named offline models, immutable/versioned integer fee schedules, partial-fill reports with model/source/quality provenance, instrument-bound access to the legacy internal CLOB, and a double-entry `PortfolioLedger`. The default fee schedule is explicitly `NO_FEES_CONFIGURED`; BASALT does not ship assumed or current statutory rates. Fee percentages round upward per line, fixed amounts and caps are applied per fill, and GST applies to fee components marked GST-eligible. Configure a selected schedule before drawing cost conclusions. Order-book replay currently sweeps recorded depth; it does not synthesize queue-position fills.
