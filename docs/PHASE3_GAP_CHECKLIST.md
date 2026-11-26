# BASALT Phase 3 gap checklist

This is the source and UI review recorded before Phase 3 implementation. The
repository's root `DESIGN.md` was read first. No root `skills.md` or `SKILLS.md`
was present in the tracked or untracked repository file inventory; the Product
UX Studio skill was used for the interface review. Existing user files and
changes (`.gitignore`, `docs/.mcp.json`, `.agents/`, `DESIGN.md`, `kraken/`, and
`skills-lock.json`) are outside this work and must remain untouched.

## Security engineering and verification

| Area | Existing baseline | Phase 3 gap / action |
|---|---|---|
| Thread safety | Linux Clang TSan is a blocking CI job. SPSC queue, market event bus, event log writers and kill switch have concurrent regression tests. | Add concurrent operations through the mutex-serialized `SimulationFacade` boundary; preserve CI as blocking. Local Windows toolchain cannot run TSan. |
| Fuzzing | CI has 30-second libFuzzer smoke jobs for EventLog recovery, CSV parsing, generic JSON parsing and MCP frames. The CMake FetchContent dependencies use immutable commit IDs. | Add meaningful order-book state-transition fuzzing and direct HTTP mutation-schema fuzzing. The generic JSON fuzzer does not exercise BASALT's schema boundary. Extend the regression corpus and CI smoke list. |
| Persistence recovery | Tests cover valid prefixes, torn/corrupt tails, reopen, rotation, concurrent appends, checkpoints and catalogs. | Add deterministic storage fault injection for writes, flushes, resize and replacement, then assert error reporting and safe recovery/reconciliation. |
| Native dependencies | Three C++ dependencies are pinned to immutable commits and frontend dependencies are lockfile-pinned. Existing OSV scanning covered npm. | Verify the declared CMake pin set in a regression check and produce a CycloneDX inventory covering native and frontend dependencies. |
| Security docs | Phase 2 threat model, security posture, audit and testing documents describe implemented controls and local limits. | Update the verification guide and audit with Phase 3 coverage and only claim checks actually run. |

## UI and interaction review

The app has 13 workspaces: Overview, Market Watch, Instrument Workstation,
Order Book, Strategy Lab, Paper Trading, Portfolio & Risk, Backtesting, Market
Replay, Analytics, Data Sources, System Monitor, and Settings. Views use live
API state and honest empty states; no Phase 3 work should introduce fabricated
market or performance data.

| Area | Review finding | Phase 3 action |
|---|---|---|
| Design system | Dark palette is navy/green/blue, while `DESIGN.md` specifies near-black layered surfaces, hairlines and sparse lavender accent. Typography and many controls are 8–11px / 29–34px. | Bring shared tokens, type hierarchy and interactive target sizing closer to the design system while preserving semantic buy/sell, warning and error colors. |
| Backtesting | Strategy and execution model selects are not bound to state; submission hard-codes `strategy_id: "1"` and `BAR_APPROXIMATION_MODEL`. The current gateway does not implement REST v1 backtests. | Make the form truthful: bind supported values, disable or remove unsupported choices, and report the unavailable endpoint without implying a run can be queued. |
| Market Replay | The badge always says `NO SESSION`; command buttons remain available when no session or REST v1 API exists; speed sends a request for every selection and there is no pending state. | Reflect selected/API state, gate unsupported commands and serialize pending actions. Keep range seek from flooding requests. |
| Keyboard/accessibility | Clickable market table rows are keyboard-focusable but lack an interactive semantic role and accessible selected state. Many small controls depend on the global focus ring. | Use native interactive semantics where practical, add explicit labels/status, and improve focus visibility and minimum touch targets. |
| Responsive layout | Existing breakpoints cover desktop, tablet and narrow mobile, but small text and navigation/control heights remain difficult to use at narrow widths. | Re-check shared layout and representative high-density workspaces at narrow widths; retain horizontal table scrolling where column data requires it. |
| Rendered QA | Baseline typecheck, unit tests and production build pass. Local browser could not reach the loopback preview in this environment. | Attempt a second local render path after implementation; if the host boundary persists, record that limitation and use component-level behavior and build verification without claiming visual browser QA. |

## Baseline verification

Before implementation, the existing tree passed CTest 177/177 (loopback access
required), frontend typecheck, Vitest 10/10, and the frontend production build.
No Phase 3 source changes had been made when this checklist was recorded.
