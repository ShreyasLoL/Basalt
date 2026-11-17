# BASALT hardening checklist

This checklist tracks the supplied hardening plan. An item is complete only after its stated implementation and verification are recorded. Phase work stays on a phase-specific local branch and is committed in focused changes; nothing is pushed.

## Phase 0 — Baseline

- [x] Record the architecture and review the repository's actual documentation and CMake files.
- [x] Clean-first C++ Release build, full CTest run, warning count and elapsed time recorded.
- [x] Record frontend Vitest count for the documentation drift check.
- [x] Create this checklist with all requested work items.

Baseline run on 2026-11-17: `cmake --build build-release --clean-first --parallel` rebuilt 75 Ninja targets successfully with no warnings; command wall time was about 15 seconds. `ctest --test-dir build-release --output-on-failure` passed 143/143 in 5.04 seconds after rerunning with loopback access. The first sandboxed run passed 137/143; all six failures were HTTP clients unable to connect to the fixture's loopback listener. `npm run typecheck` passed, `npm test -- --reporter=dot` passed 10/10 across 3 files in 6.12 seconds, and `npm run build` passed (42 modules; Vite build 1.45 seconds). The repository's CMake build uses Clang 22.1.8, Ninja, and Release mode.

## Phase 1 — End-to-end path

- [x] Map actual provider, pipeline, strategy, risk, paper execution, ledger and API interfaces; identify missing glue.
- [x] Implement a deterministic synthetic-provider orchestrator.
- [x] Add a runnable demo reporting ticks, signals, orders, fills, risk rejections, ledger and PnL.
- [x] Add deterministic integration coverage for repeatability, risk rejection, kill switch and ledger/fill reconciliation.
- [x] Expose the portfolio through the specified HTTP read route and verify the response in the integration test.
- [x] Document demo invocation in README.

Phase 1 result: added `PlatformRunner`, an opt-in `HttpServer` portfolio read model (wired into the regular HTTP server after a deterministic 12-tick local session), and `basalt_demo`. Its quote-driven demonstration strategy deliberately submits over-limit orders to exercise risk rejection; it is a wiring fixture, not a strategy recommendation or performance result. Same-seed runs produce identical ledger hashes/PnL, and the full gate passed: clean-first Release build (81 Ninja targets, no warnings), CTest 144/144 (4.81 seconds), and the demo completed with 8 ticks, 6 signals, 4 accepted orders, 4 fills and 2 quantity-limit rejections.

## Phase 2 — REST v1 and WebSocket

- [x] Diff every route in `docs/API_SPEC.md` against the HTTP implementation and record status.
- [x] Implement health, market watch, instrument metadata, instrument snapshot and REST errors. WebSocket is deferred because the bundled `cpp-httplib` has no upgrade hook; health reports `websocket_available: false`, `/api/v1/stream` returns schema-versioned 501, and the frontend skips retries.
- [x] Implement supported portfolio, risk and telemetry groups; return schema-versioned `NOT_IMPLEMENTED` for unsupported v1 routes.
- [x] Preserve legacy routes and implement configurable exact-origin CORS with an empty explicit allowlist by default and correct preflight.
- [x] Add HTTP tests for implemented route shapes, unsupported routes, CORS allow/deny, malformed/oversized bodies and depth validation. A real WebSocket handshake and streamed frame remain deferred with the transport.
- [x] Verify frontend v1 mode and correct the portfolio camelCase mismatch; run typecheck, Vitest and Vite build.

### Phase 2 route audit

| API_SPEC route | Status in this build |
|---|---|
| GET `/api/v1/health` | Implemented; reports in-memory storage and WebSocket availability. |
| GET `/api/v1/market/watch` | Implemented from the deterministic SIM read model; unobserved instruments remain disconnected/null. |
| GET `/api/v1/instruments/{id}` | Implemented for the active SIM catalog. |
| GET `/api/v1/instruments/{id}/snapshot` | Implemented for quote, paper fills and frontend book shape; depth/book levels and activity are empty because the platform read model does not connect a CLOB. Depth query is validated 1–200. |
| GET `/api/v1/order-books/{id}` | 501 `NOT_IMPLEMENTED`; the platform runner does not expose a CLOB snapshot. |
| GET `/api/v1/trades` | Implemented for recent paper fills, bounded to 200. |
| POST `/api/v1/orders` | 501 `NOT_IMPLEMENTED`; legacy paper CLOB remains on `/api/orders`. |
| DELETE `/api/v1/orders/{id}` | 501 `NOT_IMPLEMENTED`. |
| PATCH `/api/v1/orders/{id}` | 501 `NOT_IMPLEMENTED`. |
| GET `/api/v1/orders` | 501 `NOT_IMPLEMENTED`. |
| GET `/api/v1/portfolio/{account_id}` | Implemented with camelCase frontend fields and null equity when marks are unavailable. |
| GET `/api/v1/risk/{account_id}` | Implemented for the active paper account; configured limits and session rejection codes are exposed, utilization remains null. |
| POST `/api/v1/strategies` | 501 `NOT_IMPLEMENTED`. |
| GET `/api/v1/strategies` | 501 `NOT_IMPLEMENTED`. |
| POST `/api/v1/strategies/{id}/commands` | 501 `NOT_IMPLEMENTED`. |
| POST `/api/v1/backtests` | 501 `NOT_IMPLEMENTED`. |
| GET `/api/v1/backtests/{id}` | 501 `NOT_IMPLEMENTED`. |
| POST `/api/v1/replay/{session}/commands` | 501 `NOT_IMPLEMENTED`. |
| POST `/api/v1/benchmarks` | 501 `NOT_IMPLEMENTED`; legacy benchmark remains on `/api/benchmark`. |
| GET `/api/v1/system/metrics` | Implemented for observed pipeline/event-bus counters; latency stages and event rate are null/unavailable. |
| WebSocket `/api/v1/stream` | Deferred: returns 501 `STREAM_UNAVAILABLE`; no upgrade or streamed-frame test is claimed. |

Phase 2 result: REST read routes now match the frontend's camelCase types, portfolio JSON was corrected, CORS supports `BASALT_CORS_ALLOWED_ORIGINS` with exact origins and no wildcard, and unsupported routes return schema-versioned errors. The WebSocket route is explicitly unavailable because the bundled transport does not support upgrades. HTTP tests cover route shapes, errors, CORS, malformed JSON, oversized payloads and bounded depth. Final clean-first Release build passed all 81 Ninja targets with no warnings; CTest passed 149/149 in 6.54 seconds; Vitest passed 10/10 in 4.86 seconds; frontend typecheck and Vite build passed (42 modules, 1.07 seconds). A WebSocket handshake and streamed frame remain deferred.

## Phase 3 — Confirmed bug and risk fixes

- [x] Unify MCP and KillSwitch persistence through one canonical event schema; document actor and reason-redaction decisions in an ADR.
- [x] Keep the portable ratio-basis-point helper and seeded/extreme regression coverage; add an O(1)-versus-legacy microbenchmark.
- [x] Unify automatic order-ID allocation across MatchingEngine and SimulationFacade; test direct, seeded, batch, explicit-ID advancement and exhaustion behavior.
- [x] Emit observable STP cancellation results and verify safe book/map erasure ordering with regression coverage.
- [x] Keep EventLog's append handle open with a per-append durable flush policy and explicit close/recover rotation boundary; test torn-write recovery and benchmark append throughput.
- [x] Add Catalog batch upsert with one read/merge/write path; test a 2,000-record import and replacement semantics.
- [x] Add a genuinely undersized arena benchmark, report deterministic exhaustion, and document the separate pressure case.

Phase 3 result: clean-first Release build passed all 81 Ninja targets with no warnings; final CTest passed 156/156 in 5.69 seconds with loopback access enabled; frontend typecheck passed, Vitest passed 10/10, and Vite built 42 modules in 1.22 seconds. The fixed-math microbenchmark matched checksums (4,000 ns for the helper and 3,566,400 ns for the legacy loop, 1,000 operations). EventLog durable append measured 9,738,620,200 ns for 1,000 appends on this Windows host. The 10,000-attempt arena pressure run reported capacity 2,500, 2,500 allocations and 7,500 exhaustions. These single-host measurements are recorded in `docs/PERFORMANCE.md`; Phase 3 is committed locally and not pushed.

## Phase 4 — Testing infrastructure

- [x] ASan+UBSan full suite passed; no reports. TSan option and Linux CI job added, but local TSan is deferred because this Windows Clang host lacks a supported runtime and WSL returns `Wsl/E_ACCESSDENIED`; remote CI has not run from this unpushed branch.
- [deferred] Fuzzer option, four libFuzzer harnesses, seed corpora, coverage instrumentation, Linux 30-second budgets and `fuzz/FINDINGS.md` are implemented. Local harness linking failed with the Windows runtime's `annotate_string` mismatch; the Linux campaigns await a compatible run. No crash-free fuzz result is claimed.
- [x] Randomized parity/invariant test covers all three order-book implementations, four profiles and two retained seeds.
- [x] CI configured for Linux Clang/GCC and Windows MSVC with C++ build/CTest, frontend gates, Linux ASan/UBSan and TSan jobs, fuzz budgets, dependency cache and README status badge. Remote workflow validation is pending because this branch is not pushed.

Phase 4 result: final Release clean build passed 81/81 targets without warnings and CTest passed 157/157 (7.77 s). Windows Clang 22.1.8 ASan+UBSan clean build passed 81/81 and CTest passed 157/157 (24.90 s) with no sanitizer reports. Frontend typecheck passed, Vitest passed 10/10 across 3 files, and Vite built 42 modules. Exact commands, Windows/WSL limitations and fuzz status are in `fuzz/FINDINGS.md`. The phase is committed locally; no push was made.

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
