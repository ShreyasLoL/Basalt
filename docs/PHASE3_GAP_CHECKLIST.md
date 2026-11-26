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
| Thread safety | Linux Clang TSan is a blocking CI job. SPSC queue, market event bus, event log writers and kill switch have concurrent regression tests. | **Done:** Added concurrent submits/cancels/snapshots through the mutex-serialized `SimulationFacade`; focused test passed 1/1 and full CTest passed 182/182. Local Windows toolchain cannot run TSan. |
| Fuzzing | CI had bounded libFuzzer smoke jobs for EventLog, CSV, JSON and MCP frames. The CMake FetchContent dependencies use immutable commit IDs. | **Done:** Added order-book state-transition, JSONL-import and production HTTP mutation-schema fuzz targets with seed corpora and CI smoke entries. Relevant regression tests passed 18/18; harness translation units compile with `clang-cl /W4 /WX`. Local fuzz campaigns remain Windows-deferred. |
| Persistence recovery | Tests cover valid prefixes, torn/corrupt tails, reopen, rotation, concurrent appends, checkpoints and catalogs. | **Done:** Added deterministic write, flush, resize and replacement failure injection with recovery/reconciliation checks; focused subset passed 4/4. |
| Native dependencies | Three C++ dependencies are pinned to immutable commits and frontend dependencies are lockfile-pinned. Existing OSV scanning covered npm. | **Done:** Added a CMake pin lock check and checked-in CycloneDX inventory for native and npm dependencies. `python scripts/dependency_sbom.py --check` passes. |
| Security docs | Phase 2 threat model, security posture, audit and testing documents describe implemented controls and local limits. | **Done:** Updated the security testing guide, audit log, frontend guide, README verification and this checklist with only observed results and current limitations. |

## UI and interaction review

The app has 13 workspaces: Overview, Market Watch, Instrument Workstation,
Order Book, Strategy Lab, Paper Trading, Portfolio & Risk, Backtesting, Market
Replay, Analytics, Data Sources, System Monitor, and Settings. Views use live
API state and honest empty states; no Phase 3 work should introduce fabricated
market or performance data.

| Area | Review finding | Phase 3 action |
|---|---|---|
| Design system | Dark palette is navy/green/blue, while `DESIGN.md` specifies near-black layered surfaces, hairlines and sparse lavender accent. Typography and many controls are 8–11px / 29–34px. | Bring shared tokens, type hierarchy and interactive target sizing closer to the design system while preserving semantic buy/sell, warning and error colors. |
| Backtesting | Strategy and execution model selects were not bound; submission hard-coded `strategy_id: "1"`. The current gateway returns `501 NOT_IMPLEMENTED` for the documented backtest route. | **Done:** Dataset and strategy come from actual API state; only the supported bar-approximation model is shown. The client displays server errors and invents neither success nor metrics. Payload behavior is covered with a controlled API test. |
| Market Replay | The badge always said `NO SESSION`; commands were enabled without a session or REST v1 API; speed sent on every selection and had no pending state. | **Done:** Session/API state is visible; commands require REST v1 and a nonempty ID, pending actions serialize, speed applies explicitly, and seek submits an event sequence on demand. Legacy gating and command payloads are tested. |
| Keyboard/accessibility | Clickable market rows lacked native interactive semantics and accessible selected state. Many small controls relied on the global focus ring. | **Done:** Instrument selection is a native button with pressed state, a skip link targets the workspace, error notifications use alert semantics, controls have visible focus and larger targets, and BUY/SELL are labeled pressed-state controls. |
| Responsive layout | Existing breakpoints covered desktop, tablet and mobile, but text and navigation/control heights were small. | **Done by source review:** Updated shared tokens, type scale, 40px controls, narrow navigation, responsive metric grids and stacked seek controls; tables retain horizontal scrolling. Browser rendering was not visually checked. |
| Rendered QA | Baseline typecheck, unit tests and production build passed. Local browser could not reach the loopback preview. | **Limitation recorded:** Frontend typecheck, 16/16 Vitest tests and production build passed. No browser visual or end-to-end QA is claimed; the earlier preview path was unreachable, and the user requested no computer UI access for this turn. |

## Baseline verification

Before implementation, the existing tree passed CTest 177/177 (loopback access
required), frontend typecheck, Vitest 10/10, and the frontend production build.
No Phase 3 source changes had been made when this checklist was recorded.

## Phase 3 result

The security features are committed locally in `66a94df`, `74b67ac`,
`b014192` and `4f08daa`. The UI foundation and interaction work are committed
locally in `38b895b`, `110a273` and `9fe7f0b`. Full CTest passed 182/182; frontend
typecheck, Vitest 16/16, production build and the pin/SBOM check passed. The
Windows build could not replace the final `basalt_server.exe` because
Windows returned a permission error; no process was stopped. TSan, libFuzzer campaigns and browser
visual QA remain unclaimed locally. The user requested local commits only;
nothing was pushed.
