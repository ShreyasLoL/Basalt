# BASALT implementation progress

This is the authoritative Phase 0–10 checklist. `[x]` means the requirement was implemented and verified; `[~]` means work is underway or verification is externally blocked; `[ ]` means pending. Never mark a gate complete based on a plan, documentation, or compile-only result.

## Specification suite (18 files)

- [x] docs/documents.md — master spec and verified repository audit/addendum
- [x] README.md — overview, existing quickstart and spec links
- [x] docs/ARCHITECTURE.md
- [x] docs/ROADMAP.md
- [x] docs/DATA_MODEL.md
- [x] docs/API_SPEC.md
- [x] docs/MARKET_DATA.md
- [x] docs/STRATEGIES.md
- [x] docs/BACKTESTING.md
- [x] docs/PAPER_TRADING.md
- [x] docs/RISK.md
- [x] docs/FRONTEND.md
- [x] docs/MCP.md
- [x] docs/PERFORMANCE.md
- [x] docs/SECURITY.md
- [x] docs/TESTING.md
- [x] docs/OPERATIONS.md
- [x] docs/DECISIONS.md

## Supplied source documentation reviewed

- [x] docs/00_CODEX_PROMPT.md and docs/01_RESUME_BOOSTER.md
- [x] docs/ARCHITECTURE.seed.md, docs/AUDIT.seed.md, docs/DATA_MODEL.seed.md, docs/DECISIONS.seed.md
- [x] docs/MARKET_DATA.seed.md, docs/MCP.seed.md, docs/PAPER_TRADING.seed.md
- [x] docs/documents.md (legacy six-phase core spec retained as Phase 0 compatibility source)
- [x] Root README.md and prior PROGRESS.md
- [x] Recorded correction: seed paths include include/atlas, frontend/ and benchmarks/; actual repository uses include/basalt, web/ and src/bench. Supplied seed files are directly under docs/, not docs/seed/.
- [x] Root documents.md was already deleted in the submitted working tree. Preserve that pre-existing deletion; docs/documents.md is the current master. Do not restore without an explicit user instruction.

## Phase 0 — Forensic audit and baseline verification [x]

- [x] Audit CMake, all tracked source/header/test/UI files, VS Code tasks, README and supplied docs before source changes.
- [x] Release build: CMake 4.3.1, Ninja generator invoked via cmake --build, Clang 22.1.8 x64 Windows target; strict project warning flags; build succeeded (22 steps).
- [x] Existing GoogleTest suite: CTest passed 18/18 (0 failures, 0.49 s).
- [x] Existing benchmark: all seven profiles at 50,000 commands/profile; MapListOrderBook and VectorOrderBook produced matching trade/cancel counts and final snapshots. Raw measurements in docs/PERFORMANCE.md.
- [x] Static route/call-path audit: GET /api/state, POST /api/orders, DELETE /api/orders/:id, POST /api/sim/step, POST /api/reset, POST /api/benchmark; root page served by static/embedded UI path.
- [x] Runtime smoke on temporary loopback port: GET / and /api/state returned 200; POST /api/orders returned NEW; DELETE cancel succeeded; /api/sim/step executed 2; /api/reset cleared and reseeded; /api/benchmark returned 2 parity rows.
- [x] Legacy aliases /api/seed and /api/simulate were probed and return 404. The repository-grounded contract uses POST /api/reset with seed_demo and POST /api/sim/step; no alias requirement is supported by the legacy specification.
- [x] Documentation suite committed locally before Phase 0 completion: d67e745.
- [x] Phase 0 baseline and audit committed locally; see commit history. No push.

## Phase 1 — Foundation, configuration, logging and code quality [x]

- [x] Common fixed-point types, checked add/multiply, Result/Error, source and quality enums under basalt::common; legacy aliases remain untouched.
- [x] Validated environment/CLI configuration defaults to 127.0.0.1:8080, rejects remote bind without explicit opt-in, and checks ports, queues and stale timeout.
- [x] Structured JSON/console logger redacts sensitive field names and embedded token/password/API-key assignments.
- [x] Added basalt_common_tests and opt-in BASALT_ENABLE_SANITIZERS CMake switch; preserved all legacy interfaces and tests.
- [x] CMake Release build succeeded with strict warnings; CTest passed 26/26 (18 legacy + 8 common); environment-configured loopback server smoke succeeded on port 18765.
- [x] Phase 1 implementation committed locally; no push.

## Phase 2 — Institutional workstation frontend [x]

- [x] React + TypeScript + Vite + Tailwind + Zustand + Lightweight Charts and package lock; ignored npm cache and generated `web/app/` output per frontend spec.
- [x] 13-workspace dark responsive workstation, typed quote/book/tape/order views, chart empty state, keyboard-operable navigation, risk controls and truthful empty/unavailable states.
- [x] Typed REST v1 and legacy CLOB adapters; integer tick labels preserved; unsupported advanced order types disabled on legacy server.
- [x] WebSocket v1 frame validation, 20 Hz quote coalescing, bounded 512-event store, durable-frame queue, last-sequence resume, reconnect backoff and REST snapshot request on explicit gap/overflow.
- [x] C++ gateway prefers the built bundle and serves `/assets/*`; retains `web/index.html` fallback. README documents `npm ci`/typecheck/test/build.
- [x] Verification: frontend typecheck passed; Vitest passed 7/7; Vite production build succeeded; `npm install` reported 0 vulnerabilities; C++ Release build succeeded and CTest passed 26/26.
- [x] Runtime smoke: loopback GET `/` served React root, generated JS/CSS returned 200, GET `/api/state` returned the seeded book; browser check confirmed Market Watch tick units and legacy order constraints.
- [x] Phase 2 committed locally; see commit log. No push.

## Phase 3 — Market-data gateway and instrument master [x]

- [x] Versioned InstrumentMaster supports NSE/BSE/SIM definitions and effective-dated provider mappings; ten SIM symbols are seeded without inventing NSE/BSE feed tokens.
- [x] Added provider-neutral `IMarketDataProvider`, canonical event types, fixed-point/tick/lot validation, gap/duplicate/out-of-order detection, stale and clock-skew quality, reconnect generation, and bounded event bus.
- [x] Deterministic synthetic SIM provider plus bounded CSV/JSON Lines stream replay; parsers reject malformed/oversize records and do not open arbitrary paths.
- [x] Upstox v3 and FYERS diagnostic-only adapters remain disconnected, read no credential values, have no live capabilities and expose no order methods.
- [x] Verification: Release build passed with strict warnings; CTest passed 38/38 (18 legacy + 8 common + 12 market-data); focused coverage includes fixed-point/tick, malformed events, aliases, stale/skew, dedup/order/gap/reconnect, replay parsing, queue pressure and diagnostic adapters.
- [x] Phase 3 locally committed; provider connections remain intentionally diagnostic-only pending official schema/entitlement/licensing review. No push.

## Phase 4 — Persistence, historical storage and event log [x]

- [x] SQLite is unavailable in the configured Windows/Clang toolchain; implemented the roadmap's deterministic, durable file-store fallback instead of an in-memory substitute. Catalog JSON uses sorted datasets, atomic replacement, SHA-256 migration checksums, and a tested v0→v1 migration.
- [x] Added append-only `.blog` frames with little-endian length/version/sequence/CRC, bounded payloads, strict sequence checks, valid-prefix recovery, damaged-tail quarantine, and post-recovery CRC validation on reads.
- [x] Added checksummed atomic checkpoints, checkpoint-plus-log deterministic replay manifests, fixed-point OHLCV CSV import/export, canonical dataset SHA-256, and license/provenance/range metadata.
- [x] Verification: strict Release build succeeded; CTest passed 48/48, including legacy matching-engine tests and persistence restart, migration, torn/corrupt-tail, checkpoint, replay determinism, CSV validation and hash-tamper cases.
- [x] Implementation details and file formats recorded in `docs/PERSISTENCE.md`. Phase 4 committed locally; no push.

## Phase 5 — Risk, paper execution and portfolio ledger [x]

- [x] Sixteen risk checks with stable rejection codes; durable decision audit, restart idempotency/order-rate recovery, reservations, and a latched global/account/strategy kill switch.
- [x] Four offline paper execution models with explicit source/model/fee-schedule provenance, stale/gap checks, conservative slippage and participation limits.
- [x] Integer-only versioned fee calculator and double-entry cash/position ledger with partial-fill, short-flip, fee, conservative mark and restart recovery support.
- [x] Verification: strict Release build succeeded; CTest passed 68/68 (18 legacy + 8 common + 12 market-data + 11 persistence + 19 trading). Targeted coverage includes all sixteen rejection codes, risk boundaries, audit failure, kill-switch recovery, fee rounding, four execution models, partial fills, short flips, restart and reconciliation.
- [x] Phase 5 implementation committed locally; no push.

## Phase 6 — Strategy runtime and six quantitative strategies [x]

- [x] Ten-state lifecycle with typed transition errors, instrument/context validation, warm-up suppression, stale/gap/quality filtering, pause/resume/stop, and exception isolation through a host failure sink.
- [x] Incremental integer-only SMA, EMA, Wilder RSI, population-variance Bollinger bands with floor integer square root, checked session VWAP, and signed top-five book imbalance.
- [x] Six deterministic built-ins: SMA crossover, RSI mean reversion, configurable Bollinger breakout/reversion, intraday VWAP reversion, prior-channel momentum with completed-bar volume confirmation, and top-five order-book imbalance.
- [x] Strategy descriptors expose stable IDs/version/hash, required channels/warm-up and parameter schema; unknown parameters fail closed. Budget sizing accounts for configured fees/slippage, rounds to whole lots, and close quantity cannot exceed the open position.
- [x] Runtime routes generated intents through Phase 5 RiskEngine; only approved signals are returned, and rejection decisions remain inspectable.
- [x] Verification: strict Release build succeeded; CTest passed 88/88 (18 legacy + 8 common + 12 market-data + 11 persistence + 19 trading + 20 strategy). Fixture coverage includes indicator math/overflow, all six strategy signals, deterministic IDs, session resets, risk acceptance/rejection, warm-up, stale/gapped inputs, legal transitions and exception isolation.
- [x] Phase 6 implementation committed locally; no push.

## Phase 7 — Bias-free backtesting and deterministic replay [x]

- [x] Chronological single-instrument dispatcher rejects invalid, stale, gapped, skewed, unordered, overlapping, off-tick and out-of-split bars; bar-close signals can fill only at a subsequent bar open with configured adverse spread/slippage, participation, fees and risk controls.
- [x] Half-open train/validation/test ranges and embargo; reproducibility manifest covers source revision/dirty state, compiler/build metadata, strategy/config/dataset/event hashes, instrument, initial portfolio, execution, risk and survivorship/corporate-action/license assumptions.
- [x] Replay play/pause/step/seek/1x/5x/10x/MAX and verified checkpoint restoration with event replay.
- [x] Exactly eighteen analytics metrics preserve undefined values with explicit statuses; result includes signal/order/fill/position/cash journals and deterministic input/output/manifest hashes.
- [x] Verification: strict Release build succeeded; CTest passed 97/97 (18 legacy + 8 common + 12 market-data + 11 persistence + 19 trading + 20 strategy + 9 backtest). Cases cover split embargo, replay controls/checkpoints, duplicate and unordered input, all 18 metrics and undefined states, next-bar timing, deterministic hashes, look-ahead/dataset leakage, seeded positions, risk-rejection journaling and invalid quality input.
- [x] Phase 7 committed locally; no push.

## Phase 8 — Multi-instrument matching engine evolution [x]

- [x] Additive `MultiInstrumentEngine` maintains instrument-isolated books with globally unique active order IDs, instrument/account/participant trade attribution, cancel and invariant inspection; legacy `MatchingEngine` is unchanged.
- [x] LIMIT, MARKET, IOC, FOK, POST_ONLY, STOP_MARKET and STOP_LIMIT, reference-trade stop activation, and amendments. FOK failure returns before book/sequence/trade-ID mutation; quantity decreases retain priority while price/quantity increases requeue at the tail.
- [x] Account/participant STP policies implement CANCEL_NEWEST, CANCEL_OLDEST and CANCEL_BOTH.
- [x] Verification: strict Release build succeeded; CTest passed 106/106 (18 legacy + 8 common + 12 market-data + 11 persistence + 19 trading + 20 strategy + 9 backtest + 9 matching-evolution). Focused invariant, order-type, amendment, STP, FOK atomicity and instrument-isolation fixtures pass.
- [x] Semantics recorded in `docs/MATCHING_ENGINE.md` and `docs/TRADING.md`; Phase 8 committed locally; no push.

## Phase 9 — Telemetry, twelve workloads and memory arena [x]

- [x] Bounded sampled `PipelineTelemetry` stores eleven monotonic timestamps, reports ten adjacent T0–T10 spans plus end-to-end percentiles (p50/p90/p95/p99/p99.9), clock source/resolution, queue high-water, feed-quality counts, sampled spans and capacity/validation drops.
- [x] Twelve seeded profiles cover balanced, buy-heavy, sell-heavy, aggressive, cancellation-heavy, concentrated, wide, bursty, high-active-order, fifty-instrument, normalized market-data burst and slow-consumer backpressure cases.
- [x] Fixed-capacity generation-checked `ObjectPool<Order>` and intrusive `ArenaOrderBook`; benchmark runs compare full trade sequences, cancellations and final per-instrument snapshots with map/list and vector books. Pool capacity, acquisitions, peak use, exhaustion, durable queue high-water and accepted/drop counts are reported.
- [x] Verification: strict Release build succeeded; CTest passed 112/112 (18 legacy + 8 common + 12 market-data + 11 persistence + 19 trading + 20 strategy + 9 backtest + 9 matching evolution + 6 telemetry/benchmark). Pool exhaustion, stale handles, reclamation, telemetry sampling/drop accounting and all-profile parity fixtures passed.
- [x] Ran `basalt_bench --orders 10000` and recorded the actual 12-profile output and observed Windows/CPU/compiler/build metadata in `docs/PERFORMANCE.md`; results are explicitly single-host measurements, not guarantees. Phase 9 committed locally; no push.

## Phase 10 — BASALT MCP server [x]

- [x] JSON-RPC 2.0 newline-delimited stdio lifecycle with initialize negotiation, initialized notification, tools/list, tools/call, ping, cancellation notification consumption, bounded shutdown and frame-size enforcement; `--mcp-stdio` bypasses HTTP/provider environment configuration.
- [x] Nine read-only tools and five bounded simulation tools use closed schemas. The HTTP and MCP adapters share the thread-safe `SimulationFacade`; disconnected provider, ledger and strategy read models report `available: false` instead of fabricated data.
- [x] Inline-bar paper backtest (1,000-bar cap), in-memory replay (500 events/call), allowlisted verification suite, twelve-profile/10,000-operation benchmark and durable non-clearable kill switch. Read rate limit 120/min, simulation limit 10/min, benchmark/kill limit 5/min; controlled requests are idempotent for a 256-ID cache.
- [x] Redacted JSONL audit with one 1 MiB backup and a 1 MiB kill-switch event-log cap. No shell, arbitrary file, environment, credentials, or live-order capability; kill-switch reason stays redacted in audit, persistent log and response.
- [x] Verification: strict Release build of all targets succeeded; CTest passed 123/123 (112 existing + 11 MCP); MCP executable stdio smoke exchanged initialize/list/shutdown and reported 14 tools without HTTP configuration; loopback HTTP smoke passed state, submit, cancel, simulation step and reset. No compiler warnings; Ninja reported its existing `.ninja_log` premature-EOF recovery.
- [x] Phase 10 implementation committed locally; no push.

## Post-Phase-10 hardening patch set

Each item is one local commit; verification lines record only commands actually run (strict Release build in `build/`, Clang 22.1.8/Ninja, `ctest --test-dir build -C Release`, and `npm run typecheck`, `npm test`, `npm run build` in `frontend/`).

- [x] 1. HTTP server hardening — removed wildcard CORS; pre-routing `validate_request_origin` enforces loopback Host (403 `FORBIDDEN_HOST`, DNS-rebinding defence), allowlisted Origin when present (403 `FORBIDDEN_ORIGIN`), `application/json` for POST/PUT/PATCH/DELETE (415); OPTIONS answers 204 with allowances only for allowlisted origins; `/api/benchmark` `order_count` capped at 10,000 (default 10,000) with 400 otherwise; legacy UI benchmark selector now offers 1k/5k/10k. HTTP server moved into the `basalt_http` library; new `basalt_http_tests` target (6 pure-policy + 6 real loopback HTTP tests on an ephemeral port). Verified: build clean, CTest 135/135, Vitest 7/7, typecheck and Vite build passed; manual `basalt_server --port 18766` smoke returned 200 for `/` and `/api/state`, 403 for a foreign Host, 415 for `text/plain` reset, 200 for JSON reset, and a 204 preflight echoing `http://localhost:18766`.
- [x] 2. Risk idempotent replay safety — in `RiskEngine::evaluate_unlogged`, a same-signature retry now checks `kill_switch_.active(account_id, strategy_id)` (`KILL_SWITCH_ACTIVE`), `data_quality_ok` / `data_age_ns <= max_stale_age_ns` (`STALE_DATA`), and `market_open` (`MARKET_CLOSED`) before returning `IDEMPOTENT_REPLAY`, leaving the original acceptance and reservation untouched. Added `RiskEngineTest.IdempotentReplayStillFailsOnActiveKillSwitchStaleDataOrClosedMarket`. Verified: Release build clean, CTest 136/136, Vitest 7/7, typecheck and Vite build passed.
- [x] 3. KillSwitch lock-order deadlock — `KillSwitch::activate` and `KillSwitch::reset` now assign sequence numbers, latch state (on activate), and copy `cancel_sink_` / `sink_` under `mutex_`, release `mutex_` while invoking the callbacks, and re-lock only to record `working_orders_cancelled`/`persisted`, update state sequence ordering, and push to `events_`. Added `KillSwitchTest.CancelSinkCallingActiveAndEngineCompleteDoesNotDeadlock` and `KillSwitchTest.ConcurrentActivationsAssignUniqueSequencesWithoutDeadlock`. Verified: Release build clean, CTest 138/138, Vitest 7/7, typecheck and Vite build passed.
- [x] 4. Backtest fill-time cash/position shortfall — `BacktestEngine::run` now stages order fills atomically (and rolls back any prior partial fills of that order) when `PortfolioLedger::apply_fill` returns `INSUFFICIENT_CASH` or `INSUFFICIENT_POSITION`, marking the order `REJECTED` with `FILL_TIME_INSUFFICIENT_CASH` / `FILL_TIME_INSUFFICIENT_POSITION`, incrementing `rejected_orders` and `risk_rejections`, calling `risk_engine.complete(client_order_id)`, and erasing the pending order while preserving ledger reconciliation; all other ledger errors still abort. Added `BacktestEngineTest.FillTimeGapUpCashShortfallRejectsOrderWithoutAbortingRun`. Verified: Release build clean, CTest 139/139, Vitest 7/7, typecheck and Vite build passed.
- [x] 5. MCP paths and kill-switch truthfulness — added `McpServer::resolve_data_dir` (`BASALT_DATA_DIR` -> running executable directory -> OS temp directory with `stderr` warning, created with owner-only permissions) so default MCP audit and kill-log paths never depend on `cwd`, while preserving explicit constructor path arguments; updated `trigger_kill_switch` to return `isError: true` with `KILL_SWITCH_NOT_PERSISTED` when `event.persisted == false` while keeping the latch active in-process and reporting that in the message. Added `McpTest.TriggerKillSwitchReportsErrorWhenNotPersistedWhileKeepingInProcessLatch` and `McpTest.ResolveDataDirUsesEnvThenExecutableDirThenTempWithWarning`. Verified: Release build clean, CTest 141/141, Vitest 7/7, typecheck and Vite build passed.
- [x] 6. Replace looped `ratio_bps` — extracted shared O(1) helpers `common::ratio_bps` and `common::clamped_ratio_bps` in `include/basalt/common/fixed_math.hpp` using `unsigned __int128` under `__SIZEOF_INT128__` and a portable 64x64->128 fallback, replacing the 10,000-iteration loops in `src/strategy/indicators.cpp` and `src/strategy/builtins.cpp` with identical floor, saturation and zero/nullopt edge-case semantics. Added `FixedMathTest.RatioBpsMatchesLegacyLoopAcrossEdgeCasesAndSeededRandomRange`. Verified: Release build clean, CTest 142/142, Vitest 7/7, typecheck and Vite build passed.
- [x] 7. Frontend vs server mismatch — throttled `/api/v1/health` re-probing in `legacy` mode to at most every 30s (or manual Retry/Refresh) with zero WebSocket or optional v1 endpoint requests between intervals; updated `ChartPanel` to skip `/bars` in legacy mode (and cache 404s) while displaying a clear `"no history in legacy mode"` message; refactored `App.tsx` to use per-field Zustand selectors and memoized workspace components (`React.memo`); updated `refresh()` in `store.ts` to merge `marketWatch`, `trades`, `activity` and `lastEventSeq` by sequence when `mode === "v1"` and `streamConnected` without a gap; added an explicit legacy compatibility banner. Added 3 Vitest tests (10/10 total). Verified: Release build clean, CTest 142/142, Vitest 10/10, typecheck and Vite build passed.
- [x] 8. Minor fixes — added `phase*-test-temp/` to `.gitignore`; updated `HistoryStore::import_csv`, `read_bars` and `parse_csv` to parse in-place from a single `std::string_view` buffer without an `std::istringstream` copy, and refactored `HistoryStore::sha256` to stream in 64-byte blocks without allocating a padded copy of the input (`HistoryTest.Sha256StreamsMultiBlockInputsAgainstKnownDigests`); updated `calculate_metrics` in `src/backtest/analytics.cpp` so `"Average Win/Loss"` returns `std::nullopt` with status `"NO_WINS"` when `win_count == 0` (and `"NO_AVERAGE_LOSS"` when losses are absent) while preserving the 18-metric invariant. Verified: Release build clean, CTest 143/143, Vitest 10/10, typecheck and Vite build passed.

## Commit log

- [x] docs(spec): generate complete BASALT architecture and specification suite — d67e745
- [x] feat(phase-0): record verified repository audit and benchmark baseline — committed locally.
- [x] feat(phase-1): add common types, validated configuration, logging and sanitizer option — committed locally.
- [x] feat(phase-2): institutional workstation frontend and static bundle integration — committed locally.
- [x] feat(phase-3): add instrument master, normalized market events, bounded bus and offline providers — committed locally.
- [x] feat(phase-4): add durable catalog, framed event log, recovery, history and replay — committed locally.
- [x] feat(phase-5): add risk controls, paper execution and portfolio ledger — committed locally.
- [x] feat(phase-6): add strategy lifecycle, indicators and six built-in strategies — committed locally.
- [x] feat(phase-7): add causal backtesting, replay and analytics — 946826f
- [x] feat(phase-8): add instrument-isolated advanced matching — a731df5
- [x] feat(phase-9): add telemetry workloads and pooled arena — 38504c5
- [x] feat(phase-10): add local MCP stdio inspection and simulation interface — c635e9a

All commits are local. Never push.
