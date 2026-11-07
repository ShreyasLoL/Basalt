# Pre-Trade Risk Engine and Kill Switch

Pipeline: Strategy -> immutable OrderIntent -> identity/instrument validation -> RiskEngine checks -> reservation/approval -> PaperExecutionRouter -> ExecutionReport -> PortfolioLedger. Risk decisions are deterministic and stored with rule ID, observed values, configured limit, data sequence and rejection code. A rejected intent creates no order and no reservation.

## Sixteen configurable checks

1. **Maximum order notional:** checked fixed-point price × quantity below account cap; market orders use conservative reference/limit cap and reject when no safe quote exists.
2. **Maximum order quantity:** quantity and lot multiple within instrument and account caps.
3. **Maximum position size:** projected signed quantity after open orders and intent within per-instrument cap.
4. **Maximum gross exposure:** projected sum of absolute marked notionals within cap.
5. **Maximum net exposure:** projected absolute signed notional within cap.
6. **Maximum concentration:** projected instrument/group exposure divided by gross exposure within configured basis-point percentage.
7. **Maximum daily loss:** realized plus conservative unrealized P&L and fees within session loss floor.
8. **Maximum drawdown:** equity drawdown from session/high-water mark within limit.
9. **Maximum open orders:** account and instrument count below bound.
10. **Maximum order rate:** rolling window orders/second below configured limit.
11. **Per-instrument limit:** instrument-specific quantity/notional and allowlist.
12. **Per-strategy limit:** strategy allocation, loss, order and exposure budgets.
13. **Stale-data rejection:** required market input is fresh and quality OK for selected execution model.
14. **Market-session check:** exchange calendar, session, auction and instrument state permit the simulated action.
15. **Duplicate-order protection:** request/client IDs are idempotent and not reused with different payload.
16. **Global/strategy kill switch:** no new intent if global, account or strategy latch is active.

Rules are configurable with safe defaults. If a required mark, quote, clock, account snapshot or calculation is unavailable/overflowed, fail closed with DATA_UNAVAILABLE or RISK_CALCULATION_ERROR. Risk code uses fixed-point authoritative values. Projected exposure includes open-order reservations to prevent concurrent over-allocation.

## Rejection codes

Stable errors include ORDER_NOTIONAL_LIMIT, ORDER_QUANTITY_LIMIT, POSITION_LIMIT, GROSS_EXPOSURE_LIMIT, NET_EXPOSURE_LIMIT, CONCENTRATION_LIMIT, DAILY_LOSS_LIMIT, DRAWDOWN_LIMIT, OPEN_ORDER_LIMIT, ORDER_RATE_LIMIT, INSTRUMENT_LIMIT, STRATEGY_LIMIT, STALE_DATA, MARKET_CLOSED, DUPLICATE_ORDER, KILL_SWITCH_ACTIVE, INVALID_INSTRUMENT, INVALID_TICK, INSUFFICIENT_CASH, INSUFFICIENT_POSITION and RISK_CALCULATION_ERROR.

## Emergency kill switch

The switch is latched and scoped GLOBAL, ACCOUNT or STRATEGY. Activation atomically stops new intents, disables strategy starts, cancels working simulated orders according to deterministic cancel policy, records actor/reason/time/sequence, and emits a system fault event. Existing fills already committed remain in the ledger. Reset requires an explicit authorized local action, reason, successful health checks, and audit event; restart never clears the latch. The MCP tool can trigger only a latch, never clear it. A kill-switch activation is safe even if feed or persistence is degraded; if the durable journal is unavailable, enter read-only/fail-stop mode and surface the unsaved status.

## C++ implementation

`basalt::trading::RiskEngine` records all sixteen named rule results on accepted requests and the passing prefix plus first stable rejection on rejected requests. It tracks accepted client IDs, order-rate timestamps, and in-process exposure/cash/position reservations under a mutex. Callers provide a snapshot that excludes those in-process reservations, but includes external open-order reservations. Market orders use ask for buy risk and bid for sell risk; missing marks and arithmetic overflow fail closed. `OrderIntent::estimated_fee_micro` must contain the selected fee schedule's conservative estimate before buy cash checks.

Pass `RiskEngine::event_log_sink` with a recovered `EventLog` to make every decision durable. Startup calls `restore` with the validated log frames before accepting requests; accepted client IDs then remain idempotent across restart. A same-signature retry for an already-accepted `client_order_id` still re-checks `kill_switch_.active(account_id, strategy_id)` (`KILL_SWITCH_ACTIVE`), feed quality/staleness (`STALE_DATA`) and `market_open` (`MARKET_CLOSED`) before returning `IDEMPOTENT_REPLAY`, so a retry never reports `accepted=true` while the switch is latched or the session/feed is invalid. If a configured audit sink fails, a new approval is rolled back and the result is `AUDIT_UNAVAILABLE`.

`KillSwitch` persists scoped latches and resets through the same `.blog` event sink. Its optional cancellation callback stops simulated working orders; cancellation and persistence outcomes are recorded separately. Activation latches before either fallible action. An unsaved activation remains latched in process and the caller must enter read-only/fail-stop mode. Reset is denied unless the durable sink succeeds, the action is authorized, and health checks pass.
