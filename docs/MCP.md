# BASALT MCP Server and Figma Workflow

## BASALT MCP boundary

The local Model Context Protocol server exposes JSON-RPC 2.0 over newline-delimited stdio by default. Optional loopback SSE is not implemented. stdio stdout is reserved for protocol frames; diagnostics go to stderr. The server never exposes shell execution, arbitrary file access, environment dumps, token values or live broker order placement. The HTTP and MCP adapters use the same thread-safe `SimulationFacade` for matching-engine snapshots and simulation operations; the facade returns value objects and keeps mutable engine state private. Provider-backed market data, paper ledger and strategy runtime reads report `available: false` until those services are connected to this process.

### Nine read-only tools

1. get_system_health — build, process, storage and event-bus state.
2. get_market_watch — bounded instrument quotes with source and quality.
3. get_instrument_snapshot — instrument metadata, snapshot and freshness.
4. get_order_book — explicitly choose internal simulated book or external observed depth.
5. get_recent_trades — simulated executions with model/source context.
6. get_paper_positions — account positions and conservative marks.
7. get_portfolio_summary — cash, equity, P&L and fees.
8. get_strategy_status — config hash, lifecycle, signal and last error.
9. inspect_feed_health — provider/instrument state, gaps, age and counters.

### Five bounded simulation tools

1. run_backtest — runs one of six built-in strategies against 3–1,000 caller-supplied inline OHLCV bars using deterministic paper-only bar execution and a fixed held-out test split.
2. run_replay — starts one in-memory replay from 3–1,000 inline bars, then steps or plays at most 500 events per call at 1x/5x/10x/MAX.
3. run_test_suite — runs the named matching-invariant and/or eighteen-metric catalog checks from an allowlist; never accepts a command string.
4. run_benchmark — selects one of twelve seeded workloads and map/list, vector, arena or all implementations with a strict 10,000-operation cap; rate limited to five calls/minute per client.
5. trigger_kill_switch — durably latches a global/account/strategy simulation kill switch with a required reason; the reason is redacted in audit and persistence, and MCP cannot clear the latch. If durable persistence fails (`persisted == false`), `trigger_kill_switch` returns `isError: true` with code `KILL_SWITCH_NOT_PERSISTED` while keeping the kill switch latched active in-process and stating that in the error message.

Tool schemas are closed objects with required types, enum/range constraints and fixed operation caps. Read tools are limited to 120 calls/minute per stdio client; simulation tools are limited to ten calls/minute, with benchmark and kill-switch calls limited to five/minute. Mutating calls are idempotent for 256 cached request IDs; a reused ID with a different request is rejected. Mutations and failures create audit records with actor, method, request-ID digest, outcome, timestamp and permitted parameter names in `<data_dir>/mcp_audit.jsonl` (and kill-switch frames in `<data_dir>/mcp_killswitch.blog`), where `<data_dir>` is resolved independent of the process working directory (`cwd`) in order: `BASALT_DATA_DIR`, the directory of the running executable, or `<os_temp_dir>/basalt-mcp` (emitting a warning to `stderr`), created with owner-only permissions. Raw request IDs, argument values, free-form reasons and unknown parameter names are never logged. Schema, rate-limit and unknown-tool failures are audited. The JSONL audit file is capped at 1 MiB and rotates one bounded backup; file permissions are owner-only where supported. The persistent kill-switch event log is also capped at 1 MiB and rejects new durable writes if the cap is reached.

## Protocol behavior

Initialize negotiates one of the supported protocol versions and advertises tools capability. `tools/list` returns the fourteen tools with closed JSON schemas and read-only/simulation annotations. `tools/call` validates arguments, enforces frame, rate and work limits, calls the application facade, and returns `structuredContent` plus safe text. Errors contain stable codes without stack traces or secret values. Shutdown is bounded by the per-tool caps; cancellation notifications are consumed, while the current stdio implementation processes each bounded tool call synchronously. Automated protocol tests cover discovery, reads, backtests, replay, allowlisted checks/benchmarks, malformed schemas, request-ID idempotency, rate limits, audit rotation/redaction, kill-switch restart persistence and denial of shell/file/live execution.

Run the local stdio service with `basalt_server --mcp-stdio`. This mode starts without loading HTTP configuration or provider credentials. Each MCP process has its own local simulation facade; its matching book is not a remote control channel to another BASALT process.

## Figma MCP

Figma MCP is a design-time workflow, not a BASALT runtime integration. Use it to inspect an authorized file's components, variables and layout when the user supplies/authorizes one. Translate tokens to checked-in frontend design tokens. It does not bypass file permissions and does not itself provide browser pixel-diff regression; use the frontend visual verification workflow for that.
