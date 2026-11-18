# BASALT Data Model and Fixed-Point Rules

## Authoritative numeric representation

All INR price, cash, fee and P&L amounts are signed 64-bit integers at PRICE_SCALE=10,000 micro-rupees per INR. Thus ₹1.00 is 10,000 units and a ₹0.05 tick is 500 units. Prices are validated against an instrument tick size before persistence or matching. Quantities are whole units; positions are signed 64-bit quantities, while live order remaining quantity is unsigned and nonzero. Never use floating point for an authoritative value. Convert external decimal strings with checked decimal parsing; round only under an explicit instrument precision policy. Use checked __int128 intermediates where available and a portable checked multiply fallback.

NotionalMicro = PriceMicro × signed Quantity. Reject overflow before mutation. Average cost is integer division toward zero with the remainder retained as a deterministic cost residual or assigned by the documented rounding policy. Every journal row has a unique event sequence and idempotency key.

## Domain entities

Common IDs are strongly typed aliases: InstrumentId, AccountId, StrategyId, OrderId, ExecutionId, EventSequence, PriceTicks, MicroUnits, Quantity. DataSourceType is LIVE_PROVIDER, DELAYED_PROVIDER, HISTORICAL, CACHED, SYNTHETIC, REPLAYED or DERIVED. FeedQualityState is OK, STALE, DEGRADED or DISCONNECTED. Order values include instrument, account, participant, strategy/client IDs, side, type, TIF, limit/stop price, original/filled/remaining quantity, priority sequence, state and source. Market events include event and receive timestamps, provider sequence, instrument, source, quality, schema version and a typed payload. Persisted events are immutable; corrections are new events referencing prior IDs.

## Active persisted record model

BASALT currently has no SQLite database or relational tables. Durable state is implemented by these formats; other domain records described below are in-memory unless a caller explicitly wires a journal sink.

| Store | Persisted records | Integrity and lifecycle |
|---|---|---|
| Dataset catalog JSON | Dataset ID, instrument, interval, source, license, range, import time, relative path and SHA-256 | Versioned deterministic document; validates and migrates v0→v1; batch upserts merge once and atomically replace once. |
| Historical CSV | Integer OHLCV bars, canonicalized by `HistoryStore` | Increasing timestamps and valid OHLC; file hash is recorded in catalog and checked before reads. |
| `.blog` event log | Generic sequenced payload frames used by selected ledger, risk, kill-switch and replay journals | `BLG1` framing, monotonic sequence and CRC; invalid suffix is quarantined on recovery. |
| Checkpoint file | Caller-encoded state blob and state hash | `BCP1` framing, CRC and atomic replacement; derived from a specific event sequence. |
| MCP audit JSONL | Bounded MCP action metadata with request-ID digest and safe parameter names | 1 MiB limit and one rotated backup; free-text values are excluded. |

The instrument master, order books, strategy configurations/runs, feed-health counters, benchmark results, and backtest results are not stored as a normalized durable database by default. The ledger and risk modules can persist selected events when constructed with an `EventLog` sink; the default in-process matching facade does not become durable merely because these value types exist. These are domain APIs, not a promise of twenty persisted entity tables. The current storage choice and revisit criteria are in [ADR-009 and ADR-010](DECISIONS.md).

All persisted timestamps are UTC epoch nanoseconds; exchange-session interpretation uses the instrument calendar and Asia/Kolkata display.

## Double-entry and position invariants

Each fill writes one execution and balanced cash postings: buy debit = notional + fees; sell credit = notional - fees. A contra account records fee revenue/expense so debits equal credits per transaction. Cash balance is the initial balance plus credits minus debits. All position changes reference the same execution and event sequence.

Let q be signed current position, f signed fill quantity, p execution price and c average cost. Same-direction fills set c'=(|q|c+|f|p)/(|q|+|f|). Opposite-direction fills close min(|q|,|f|) at p and realize (p-c)×closed×sign(q); if the fill crosses through zero, the residual opens at p. Position quantity becomes q+f. Fees are posted once per fill and attributed by the selected cost schedule. Long unrealized P&L marks at best bid; short P&L marks at best ask. Missing quote uses a clearly identified fallback mark and never invents a quote.

Reconciliation: initial cash plus net ledger cash flows equals cash balance; position quantity equals sum signed fills; realized P&L equals closed quantity times price difference less allocated fees; ledger debits equal credits; event sequence and execution IDs are unique; a replay from checkpoint plus log reproduces state hash.
