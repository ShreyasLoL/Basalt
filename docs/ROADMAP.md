# BASALT Phases 0–10

This roadmap controls the 2026 platform program. Each phase is a releasable local commit. The legacy engine's Phase 1–6 in docs/documents.md describes its original history only. Every state-changing phase preserves the eighteen compatibility tests and adds focused tests. No phase enables live brokerage execution.

## Phase 0 — Forensic audit and baseline

- **Prerequisite:** repository available.
- **Scope/files:** CMake, source, tests, server/UI audit; PROGRESS.md, PERFORMANCE.md, docs/documents.md.
- **Acceptance:** record compiler/CMake/generator, strict Release build, test count, all seven baseline workload outputs, route contracts and mismatches. Verify invariants and executable endpoints where loopback access is available; otherwise capture the sandbox failure and complete static call-path audit.
- **Rollback:** documentation only; restore only Phase 0 additions if the audit is wrong.

## Phase 1 — Foundation, configuration, logging, code quality

- **Prerequisite:** Phase 0 baseline.
- **Scope/files:** include/basalt/common fixed-point/result types, validated config defaulting to 127.0.0.1:8080, structured redacting logger; CMake and additive tests. Preserve old core aliases and headers.
- **Acceptance:** config rejects invalid ports/scales/queue bounds; checked arithmetic handles overflow; logger redacts token/password/secret fields; legacy targets still compile and all legacy tests pass.
- **Rollback:** remove new common module and target wiring; leave core API unchanged.

## Phase 2 — Institutional workstation frontend

- **Prerequisite:** Phase 1 contracts and local REST read model.
- **Scope/files:** frontend/ React + TypeScript + Vite + Tailwind + Zustand + Lightweight Charts; dashboard workspaces, responsive dark tokens, keyboard navigation, bounded external event store; server static bundle integration and legacy HTML fallback.
- **Acceptance:** Market Watch, instrument chart/depth/tape, strategy lab, paper blotters/risk, backtest/replay, analytics and system monitor consume typed real API data. Empty/unavailable data is visibly empty; no fabricated performance figures. Frontend build succeeds and API remains usable offline.
- **Rollback:** disable bundle serving and keep current web/index.html available.

## Phase 3 — Market-data gateway and instrument master

- **Prerequisite:** common types, provider terms review.
- **Scope/files:** InstrumentMaster; IMarketDataProvider; twelve-stage normalizer; synthetic and CSV/JSON replay providers; safe Upstox/FYERS adapter boundaries. Secrets come only from environment or protected local config. No provider code can place orders.
- **Acceptance:** deterministic offline fixtures; malformed, duplicate, out-of-order, stale, reconnect and gap tests; all events carry source and quality labels; optional adapters run diagnostic-only when unauthenticated.
- **Rollback:** select the deterministic synthetic provider and disable external adapters.

## Phase 4 — Persistence, history, event log

- **Prerequisite:** event schemas frozen and Phase 3 canonical events.
- **Scope/files:** SQLite WAL catalog, append-only .blog frames with length/version/sequence/CRC, checkpoint/recovery, bars/datasets/export. SQLite may be bundled or optional if the platform lacks it; fallback is a deterministic transactional file store, never a silent in-memory substitute.
- **Acceptance:** reopen persisted catalog; recover valid log prefix; reject/truncate corrupt tail according to policy; snapshot plus replay yields identical state and event hash.
- **Rollback:** disable writes and load read-only last valid checkpoint/log; preserve files for recovery.

## Phase 5 — Risk, paper execution, ledger

- **Prerequisite:** canonical quotes, instruments, persistence and order intent.
- **Scope/files:** sixteen risk controls; kill switch; four paper execution models; integer-only fee and double-entry ledger.
- **Acceptance:** every rejection code and risk boundary tested; conservative quote model uses ask for buys and bid for sells; partial fills, fees, short flips, realized/unrealized P&L reconcile exactly; no broker execution path exists.
- **Rollback:** latch kill switch, cancel simulated working orders, retain immutable journal.

## Phase 6 — Strategy runtime and six strategies

- **Prerequisite:** Phase 3 events, Phase 5 risk/router.
- **Scope/files:** ten-state lifecycle, fault isolation, incremental indicators and six built-in strategies.
- **Acceptance:** transition table enforced; warm-up prevents premature signals; strategies can emit only OrderIntent; exception moves strategy to FAILED without stopping domain service; fixtures reproduce signals bit-for-bit.
- **Rollback:** stop and isolate strategies; continue feed and inspection in read-only mode.

## Phase 7 — Backtesting and deterministic replay

- **Prerequisite:** event log/history, strategies, risk, paper ledger.
- **Scope/files:** chronological dispatcher, train/validation/test ranges, replay controls, manifests and eighteen analytics metrics.
- **Acceptance:** bar-close signals fill no earlier than next bar open plus adverse cost; same dataset hash/config/seed yields identical event, trade and ledger hashes; split leakage tests pass.
- **Rollback:** mark incomplete runs invalid and replay from last verified checkpoint.

## Phase 8 — Multi-instrument engine evolution

- **Prerequisite:** compatibility layer and domain instrument IDs.
- **Scope/files:** per-instrument books, IOC/FOK/Post-Only/Stop types, modify, STP policies, account/participant IDs.
- **Acceptance:** no cross-instrument matching; FOK pre-check is non-mutating; priority rules and every STP branch tested; invariants hold after all operations; old MatchingEngine tests remain unchanged and pass.
- **Rollback:** route only supported limit/market types to legacy engine and reject advanced semantics explicitly.

## Phase 9 — Performance telemetry and memory arena

- **Prerequisite:** correctness tests and benchmark parity.
- **Scope/files:** T0–T10 latency spans; twelve deterministic workloads; baseline vector, map/list and pooled arena book comparison; allocation and queue telemetry.
- **Acceptance:** actual host/compiler metadata and percentiles recorded; all implementations match logical outputs; arena ownership, exhaustion and reclamation tests pass; no performance number is fabricated or treated as a guarantee.
- **Rollback:** switch allocator to standard containers; telemetry remains enabled.

## Phase 10 — BASALT MCP inspection interface

- **Prerequisite:** stable read model, replay/backtest facades, audit logger.
- **Scope/files:** MCP JSON-RPC stdio server, tool schemas, rate limiting, audit file, local config, automated discovery/call tests.
- **Acceptance:** nine read-only tools and five bounded simulation tools; tools/call validates schema and limits; no shell, arbitrary file, credential, or live order tool; logs redact secrets; stdio works without provider credentials.
- **Rollback:** disable MCP process and retain local UI/API operation.

## Cross-phase exit gate

For each phase: build the Release configuration, run CTest, inspect warnings, verify the relevant behavior, update PROGRESS.md with evidence and commit locally. If a check fails, fix it before advancing. A blocked external service or unavailable optional dependency is recorded truthfully, while offline behavior remains testable. No push is permitted.
