# BASALT hardening checklist

This checklist tracks the supplied hardening plan. An item is complete only after its stated implementation and verification are recorded. Phase work stays on a phase-specific local branch and is committed in focused changes; nothing is pushed.

## Phase 0 — Baseline

- [x] Record the architecture and review the repository's actual documentation and CMake files.
- [x] Clean-first C++ Release build, full CTest run, warning count and elapsed time recorded.
- [x] Record frontend Vitest count for the documentation drift check.
- [x] Create this checklist with all requested work items.

Baseline run on 2026-11-17: `cmake --build build-release --clean-first --parallel` rebuilt 75 Ninja targets successfully with no warnings; command wall time was about 15 seconds. `ctest --test-dir build-release --output-on-failure` passed 143/143 in 5.04 seconds after rerunning with loopback access. The first sandboxed run passed 137/143; all six failures were HTTP clients unable to connect to the fixture's loopback listener. `npm run typecheck` passed, `npm test -- --reporter=dot` passed 10/10 across 3 files in 6.12 seconds, and `npm run build` passed (42 modules; Vite build 1.45 seconds). The repository's CMake build uses Clang 22.1.8, Ninja, and Release mode.

## Phase 1 — End-to-end path

- [ ] Map actual provider, pipeline, strategy, risk, paper execution, ledger and API interfaces; identify missing glue.
- [ ] Implement a deterministic synthetic-provider orchestrator.
- [ ] Add a runnable demo reporting ticks, signals, orders, fills, risk rejections, ledger and PnL.
- [ ] Add deterministic integration coverage for repeatability, risk rejection, kill switch and ledger/fill reconciliation.
- [ ] Document demo invocation in README.

## Phase 2 — REST v1 and WebSocket

- [ ] Diff every route in `docs/API_SPEC.md` against the HTTP implementation and record status.
- [ ] Implement health, market watch, instrument snapshot and schema-correct WebSocket stream.
- [ ] Implement remaining strategy, portfolio, risk, dataset and metrics groups where supported; return documented spec-correct errors elsewhere.
- [ ] Preserve legacy routes and implement configurable same-origin/allowlist CORS with strict default and correct preflight.
- [ ] Add HTTP tests for routes, JSON shape, CORS, malformed/oversized requests, WebSocket handshake and a streamed message.
- [ ] Verify frontend v1 mode and correct any client mismatch; run typecheck and Vitest.

## Phase 3 — Confirmed bug and risk fixes

- [ ] Unify MCP and KillSwitch persistence through one canonical event schema; document actor and reason-redaction decisions in an ADR.
- [ ] Replace ratio-basis-point loops with a portable multiply/divide helper; add randomized/extreme tests and a microbenchmark.
- [ ] Unify automatic order-ID allocation across MatchingEngine and SimulationFacade; test collision freedom.
- [ ] Emit observable STP cancellation results and verify safe book/map erasure ordering; add regression coverage.
- [ ] Keep EventLog's file handle open with defined flush policy and rotation/reopen behavior; test torn-write recovery and benchmark.
- [ ] Add Catalog batch upsert or append journal/compaction; test large imports and complexity.
- [ ] Add a genuinely undersized arena benchmark, report exhaustion, and correct README claims.

## Phase 4 — Testing infrastructure

- [ ] Run full ASan+UBSan suite and TSan where supported; fix findings and record exact commands/platform limits.
- [ ] Add `BASALT_ENABLE_FUZZERS`, libFuzzer harnesses and seed corpora for blog recovery, CSV, JSON and MCP frames; run meaningful budgets, fix crashes/hangs, retain regressions and summarize in `fuzz/FINDINGS.md`.
- [ ] Add randomized parity/invariant tests across all three order-book implementations.
- [ ] Add CI for Linux Clang/GCC and Windows when feasible, including build, CTest, sanitizer and frontend gates; cache dependencies and add README badge.

## Phase 5 — Documentation drift

- [ ] Recount Vitest tests and correct `docs/FRONTEND.md`, `PROGRESS.md` and the README verification table.
- [ ] Remove leftover ATLAS-era branding from `web/index.html`.
- [ ] Correct `docs/MCP.md` launch/configuration instructions and remove references to nonexistent MCP files/directories.
- [ ] Reconcile SQLite WAL claims in `docs/DECISIONS.md` and `docs/PERSISTENCE.md` with an ADR for the JSON/event-log fallback and revisit criteria.
- [ ] Add `docs/THREAT_MODEL.md` covering MCP/HTTP assets, trust boundaries, attacker model, STRIDE-style threats, mitigations and residual risks.

## Phase 6 — Architecture improvements

- [ ] Write a short persistence design note; choose the smaller honest option (implement SQLite or trim the 20-entity target) and record an ADR.
- [ ] Add an optional editable example fee schedule; keep default cost-free behavior and visibly warn in backtest output when no schedule is loaded.
- [ ] Add multi-instrument backtesting with shared capital, per-instrument/portfolio risk and combined ledger; include integration test and demo.
- [ ] Add a correctly ordered, cache-line-padded SPSC event ring; TSan stress test and honest benchmark against mutex implementation.
- [ ] Replace remaining ledger checked arithmetic with the shared wide helper where simpler, retaining tests.

## Phase 7 — Final audit

- [ ] Run clean build, full CTest, sanitizer, fuzzer smoke, frontend typecheck/tests and both demos.
- [ ] Mark every checklist item done, rejected with reason, or deferred with reason.
- [ ] Report confirmed/false-positive findings, measured improvements, remaining risks and three recommended next steps.

## Phase reports

Record each phase's confirmed/rejected findings, files changed, tests added, gate results and deferred items here as work completes.
