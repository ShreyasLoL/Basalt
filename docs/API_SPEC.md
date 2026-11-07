# BASALT API v1 Contract

The gateway binds to 127.0.0.1 by default. Every JSON request and response includes schema_version: 1. Errors use {code,message,request_id,details}; secrets and provider payloads are excluded. Unknown fields are ignored only when forward-compatible; invalid enum values, over-limit payloads and missing required values return 400. State-changing calls require an idempotency/request ID and pass the same application validators as UI/MCP commands. The v1 gateway supports paper/simulation operations only.

## REST resources

| Method/path | Contract |
|---|---|
| GET /api/v1/health | Process status, version, uptime, datastore status; no credential values. |
| GET /api/v1/market/watch | Instrument, latest quote/bar, source, quality, receive/event time, stale age. Empty fields remain null. |
| GET /api/v1/instruments/{id} | Instrument metadata, tick/lot/session and provider mapping status. |
| GET /api/v1/instruments/{id}/snapshot?depth=20 | Quote, depth, book, source/quality and sequence. Depth is bounded 1–200. |
| GET /api/v1/order-books/{id}?depth=20 | Internal simulated CLOB snapshot only; external provider depth is separately labeled. |
| GET /api/v1/trades?instrument_id=&limit= | Recent paper executions, bounded limit. |
| POST /api/v1/orders | Create paper order intent; fields: request_id, account_id, instrument_id, strategy_id?, side, type, time_in_force, limit_price_micro?, stop_price_micro?, quantity, execution_model. Returns accepted/rejected plus typed risk report and order ID. |
| DELETE /api/v1/orders/{id} | Cancel paper order, idempotent cancellation result. |
| PATCH /api/v1/orders/{id} | Modify paper order under priority rules; return new priority sequence when lost. |
| GET /api/v1/orders?account_id= | Open and recent paper orders. |
| GET /api/v1/portfolio/{account_id} | Cash, positions, marks, fees, realized/unrealized P&L and data age. |
| GET /api/v1/risk/{account_id} | Current limits, utilization, kill-switch state and recent rejections. |
| POST /api/v1/strategies | Validate and create a strategy config; no implicit start. |
| GET /api/v1/strategies | Strategy definitions, versions and states. |
| POST /api/v1/strategies/{id}/commands | configure/validate/start/pause/resume/stop; runtime enforces legal transitions. |
| POST /api/v1/backtests | Bounded dataset range, strategy version, account seed, costs, execution model, split config and reproducibility metadata. |
| GET /api/v1/backtests/{id} | Run status, manifest, metrics, equity/drawdown points and warnings. |
| POST /api/v1/replay/{session}/commands | play, pause, step, seek and speed with event-sequence positions; bounded speeds only. |
| POST /api/v1/benchmarks | Workload ID, operation count and implementation set. Hard cap from validated config. |
| GET /api/v1/system/metrics | T0–T10 distributions, queue depth/drops, event rates, feed state and build metadata. |

Legacy /api/state, /api/orders, /api/reset, /api/sim/step and /api/benchmark stay available during migration. /api/seed and /api/simulate are compatibility aliases only if explicitly implemented and tested; the Phase 0 server has no such routes. The old reset body seed_demo=true and sim/step count are documented in docs/documents.md.

### Transport rules enforced by the current gateway

These apply to the implemented legacy routes, the UI and static assets (see docs/SECURITY.md for the full table):

- `Host` must be the loopback authority for the listening port (`127.0.0.1:<port>`, `localhost:<port>`, `[::1]:<port>`); otherwise 403 `{"error":"FORBIDDEN_HOST","code":"FORBIDDEN_HOST","message":...}`.
- If `Origin` is sent it must be `http://127.0.0.1:<port>`, `http://localhost:<port>` or `http://[::1]:<port>`; otherwise 403 `FORBIDDEN_ORIGIN`. Allowed origins are echoed in `Access-Control-Allow-Origin` (never `*`).
- POST and DELETE (and PUT/PATCH if added) require `Content-Type: application/json`; otherwise 415 `UNSUPPORTED_MEDIA_TYPE`. This includes bodiless calls such as `DELETE /api/orders/{id}`.
- `OPTIONS /api/*` answers 204 with method/header allowances only for an allowlisted origin.
- `POST /api/benchmark` `order_count` must be an integer 1–10,000 (default 10,000); anything else is 400.

## WebSocket v1

Upgrade path /api/v1/stream. Client sends a bounded subscribe frame with request_id, instrument IDs, channel names and optional last_event_seq. Server validates authorization/origin policy, caps subscriptions, returns subscribed or error, then streams frames:

~~~json
{"schema_version":1,"type":"quote","event_seq":42,"instrument_id":1,"event_time_ns":0,"receive_time_ns":0,"source":"SYNTHETIC","quality":"OK","payload":{"last_price_micro":0,"bid_micro":null,"ask_micro":null}}
~~~

Supported frame types: tick, quote, ohlc, depth, execution, order, position, signal, feed_status, system_metrics, replay_gap, heartbeat, error. All frames carry event_seq, source and quality when market-derived. Missing fields are null. Reconnect requests resume from last_event_seq; if durable retention cannot satisfy it, server sends replay_gap and the client refreshes a snapshot. Quotes may be coalesced; executions, order/position transitions and feed faults are never coalesced. Server sends heartbeat and closes idle/over-limit clients. Browser updates are throttled outside React render loops.

## Error codes and bounds

Stable codes include INVALID_ARGUMENT, UNKNOWN_INSTRUMENT, STALE_MARKET_DATA, RISK_REJECTED, KILL_SWITCH_ACTIVE, ORDER_NOT_FOUND, DUPLICATE_REQUEST, RATE_LIMITED, DATA_UNAVAILABLE, REPLAY_GAP and INTERNAL_ERROR. The response includes a safe human message and structured nonsecret details. JSON size, WS frame, subscriptions, history span, benchmark count and replay speed have configured finite maxima. Mutation rate limiting is per local client/session.
