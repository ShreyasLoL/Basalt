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

## Phase 1 — Foundation, configuration, logging and code quality [ ]

- [ ] Common fixed-point types, checked arithmetic, Result/Error, source and quality enums.
- [ ] Validated configuration defaults to 127.0.0.1:8080 and handles bounds/errors.
- [ ] Structured logger with secret redaction and bounded output policy.
- [ ] CMake targets and additive tests; preserve legacy interfaces and all 18 tests.
- [ ] Release build, CTest and redaction/config/arithmetic verification; commit phase.

## Phase 2 — Institutional workstation frontend [ ]

- [ ] React + TypeScript + Vite + Tailwind + Zustand + chart package and locked dependencies.
- [ ] Market Watch and instrument chart/depth/time-and-sales.
- [ ] Strategy Lab, paper blotters, portfolio/risk, kill switch.
- [ ] Backtesting, replay, analytics, data sources and system monitor.
- [ ] Typed REST/WS state, bounded external store, reconnect/gap handling, keyboard/accessibility/responsive states.
- [ ] Production bundle served by C++ gateway with legacy fallback; build/typecheck/UI verification; commit phase.

## Phase 3 — Market-data gateway and instrument master [ ]

- [ ] Versioned NSE/BSE/SIM InstrumentMaster and provider symbol mappings.
- [ ] IMarketDataProvider, canonical events and twelve-stage pipeline.
- [ ] Deterministic synthetic and CSV/JSON replay providers.
- [ ] Safe Upstox current-version/FYERS diagnostic adapters; environment secrets; no order methods.
- [ ] Tests for malformed payloads, deduplication, ordering, staleness, gaps and reconnect; build/CTest; commit phase.

## Phase 4 — Persistence, historical storage and event log [ ]

- [ ] SQLite WAL catalog, schema migrations and deterministic repositories.
- [ ] CRC-framed append-only .blog writer/reader and replay manifest.
- [ ] Historical bars/datasets/export and snapshot recovery.
- [ ] Corruption/torn-tail/restart/replay-hash tests; build/CTest; commit phase.

## Phase 5 — Risk, paper execution and portfolio ledger [ ]

- [ ] Sixteen risk checks, stable rejection codes and latched kill switch.
- [ ] Four paper execution models with explicit source/model provenance.
- [ ] Fixed-point double-entry cash, positions, fees and conservative marks.
- [ ] Risk boundary, partial-fill, short-flip, fee and reconciliation tests; build/CTest; commit phase.

## Phase 6 — Strategy runtime and six quantitative strategies [ ]

- [ ] Ten-state lifecycle, legal transitions and failure isolation.
- [ ] Incremental SMA, EMA, RSI, Bollinger, session VWAP and book imbalance.
- [ ] SMA crossover, RSI mean reversion, Bollinger, VWAP, momentum and imbalance strategies.
- [ ] Warm-up, deterministic signal, risk-only intent and exception tests; build/CTest; commit phase.

## Phase 7 — Bias-free backtesting and deterministic replay [ ]

- [ ] Chronological backtest dispatcher and next-bar execution gate.
- [ ] Train/validation/test ranges, embargo and reproducibility manifest.
- [ ] Replay play/pause/step/seek/speed and checkpoint seek.
- [ ] Eighteen metrics, determinism and look-ahead tests; build/CTest; commit phase.

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
- Phase 1–10 implementation commits pending. Local commits only; never push.
