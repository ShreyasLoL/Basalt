# Risk, paper execution and portfolio ledger implementation

The trading library is a C++20 simulation-only layer. No broker adapter or live order route is present. Risk approval is the required gate before a caller sends an `OrderIntent` to `PaperExecutionSimulator`; the simulator returns provenance-labeled fills for a caller to commit to `PortfolioLedger`.

## Risk and kill switch

Risk checks use fixed-point micro-rupee amounts and signed integer quantities. A caller supplies current account/instrument/strategy observations, open-order reservations, available cash, data age/quality, market-session status and a selected market reference. `RiskEngine` adds its own concurrent order/cash/exposure reservations and rolling order-rate history while evaluating. A matching `client_order_id` with the same full intent is an idempotent retry; changed payload under the same ID is rejected. Accepted reservations are released with `complete` when the caller records an order terminal transition.

The sixteen checks return stable rule IDs and observed/limit values. Other fail-closed errors include invalid instrument/tick, missing market reference, insufficient available cash/position, risk arithmetic overflow, and unavailable audit storage. A successful audit append is required before a configured engine returns an approval. Restart restores accepted client-order IDs from risk-decision frames. The application must supply current persisted order and portfolio snapshots when rebuilding reservations.

Kill scopes are global, account or strategy. Activation latches immediately, asks the registered cancellation callback to cancel matching simulated working orders, then attempts to append the activation event. A journal failure is surfaced through `persisted=false`, while the latch remains active. Restart calls `restore` with the validated `.blog` frames. Reset requires authorization, a reason, passing health checks, and a durable reset event; a failed append leaves the latch on.

## Execution reports and fees

`PaperExecutionSimulator::execute` validates order identity, lot and tick constraints, participation bounds, and the selected model. Conservative quote execution buys from ask and sells from bid with adverse configured ticks, capped by visible top quantity. Bar approximation checks the next bar is strictly later than the signal bar; limit fills require a strict OHLC breach and use the limit as a pessimistic price when path order is unknown. Order-book replay requires fresh, gap-free recorded depth at simulated arrival and consumes only its available levels. Internal CLOB sends only to a `MatchingEngine` explicitly bound to the same instrument ID; deploy one legacy engine instance per instrument until Phase 8 supplies the multi-instrument engine.

Each fill carries model version, source, quality, reference sequence, latency, fee schedule ID, price, quantity and integer fee. Fee components select buy/sell basis points, fixed amounts, caps and GST eligibility. Every line rounds upward in integer arithmetic. There is no default Indian-market fee table: `NO_FEES_CONFIGURED` is visibly represented until a user supplies an authoritative, versioned schedule. The Phase 8 `MultiInstrumentEngine` now supplies instrument-isolated internal books with IOC, FOK, post-only, stop, amendment and account/participant self-trade prevention semantics; the legacy `MatchingEngine` API remains compatible. See `docs/MATCHING_ENGINE.md`.

## Ledger and reconciliation

`PortfolioLedger` seeds accounts with explicit cash and short permission. A buy debits notional plus fee; a sell credits notional less fee. Matching contra postings balance each cash movement. Fills are idempotent by unique execution ID and event sequence. A configured event-log sink appends account seeds and fills before mutating in-memory state; `restore` builds a candidate ledger and installs it only after every seed and fill validates.

Positions are signed. Same-direction fills recalculate the integer average cost and retain the division remainder. Partial closes retain the remainder on the open quantity; a full close assigns it to that close, so later short flips begin at the flip fill price. Realized P&L is gross in each position; portfolio summary subtracts all incurred fees, and separately reports fees. Longs mark at bid, shorts at ask. Missing quote uses an explicitly labeled average-cost fallback with zero unrealized P&L.

`verify_reconciliation` checks event/execution uniqueness and sequence order, cash movement against notional and fees, balanced contra postings, running cash balances, net fill quantity, position fees, average-cost/residual/realized values reproduced by replaying fills, and account totals.
