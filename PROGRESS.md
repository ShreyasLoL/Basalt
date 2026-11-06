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

## Phase 8 — Multi-instrument matching engine evolution [ ]

- [ ] Isolated per-instrument books; retain single-book API and tests.
- [ ] IOC, FOK, POST_ONLY, STOP_MARKET, STOP_LIMIT, modify priority rules.
- [ ] CANCEL_NEWEST, CANCEL_OLDEST and CANCEL_BOTH self-trade prevention.
- [ ] Invariant tests for order types, modification, STP and cross-instrument isolation; build/CTest; commit phase.

## Phase 9 — Telemetry, twelve workloads and memory arena [ ]

- [ ] T0–T10 stage latency and queue/quality metrics.
- [ ] Twelve reproducible workloads including 50-instrument, market-data burst and slow-consumer.
- [ ] Fixed-capacity order pool and map/list/vector/arena parity benchmarks.
- [ ] Record real environment and run outputs; pool exhaustion/reclamation tests; build/CTest; commit phase.

## Phase 10 — BASALT MCP server [ ]

- [ ] JSON-RPC stdio lifecycle and MCP initialize/tools/list/tools/call.
- [ ] Nine read-only tools and five validated/rate-limited simulation tools.
- [ ] Redacted audit log, bounded work, no shell/file/secret/live-order capability.
- [ ] Discovery/call/schema/rate-limit/audit/redaction tests; full Release build, CTest and phase commit.

## Commit log

- [x] docs(spec): generate complete BASALT architecture and specification suite — d67e745
- [x] feat(phase-0): record verified repository audit and benchmark baseline — committed locally.
- [x] feat(phase-1): add common types, validated configuration, logging and sanitizer option — committed locally.
- [x] feat(phase-2): institutional workstation frontend and static bundle integration — committed locally.
- [x] feat(phase-3): add instrument master, normalized market events, bounded bus and offline providers — committed locally.
- [x] feat(phase-4): add durable catalog, framed event log, recovery, history and replay — committed locally.
- [x] feat(phase-5): add risk controls, paper execution and portfolio ledger — committed locally.
- [x] feat(phase-6): add strategy lifecycle, indicators and six built-in strategies — committed locally.
- Phase 7–10 implementation commits pending. Local commits only; never push.
