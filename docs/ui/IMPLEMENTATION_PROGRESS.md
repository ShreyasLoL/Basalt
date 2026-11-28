# Workstation modernization

Baseline `70c7bcd` introduced quote hierarchy, skip-link positioning and formatter reuse. The prior session reported 16 passing tests, typecheck and build.

| ID | Implementation task | Status | Commit | Checks | Outstanding |
| --- | --- | --- | --- | --- | --- |
| A | Share responsive grid, surface and control tokens | Verified | e11687c | TypeScript, production build, Vitest | See verification report for live and fixture coverage |
| B | Make all workspace destinations accessible on small screens | Verified | ae96d28 | TypeScript, production build, Vitest | See verification report for live and fixture coverage |
| C | Tie Overview to instrument selection and expose account and activity context | Verified | 11b8574 | TypeScript, production build, Vitest | See verification report for live and fixture coverage |
| D | Label book columns, distinguish missing depth, identify executions and improve ticket gating | Verified | 1a2f460 | TypeScript, production build, Vitest | See verification report for live and fixture coverage |
| E | Handle chart loading, renderer failure and retry | Verified | c456ef4 | TypeScript, production build, Vitest | See verification report for live and fixture coverage |
| F | Improve strategy lifecycle, dataset selection and analytics grouping | Verified | 4b64cb7 | TypeScript, production build, Vitest | See verification report for live and fixture coverage |
| G | Implement measured benchmark workflow | Verified | b04692b | TypeScript, production build, Vitest | See verification report for live and fixture coverage |
| H | Distinguish flat portfolio, missing risk and provider loading states | Verified | 98b07ee | TypeScript, production build, Vitest | See verification report for live and fixture coverage |
| I | Scope replay feedback and distinguish REST-only monitoring | Verified | 9269382 | TypeScript, production build, Vitest | See verification report for live and fixture coverage |
| J | Run browser, responsive, keyboard and integration checks and record findings | Verified | 1a37854 | TypeScript, production build, Vitest | See verification report for live and fixture coverage |
| K | Ignore callbacks from retired sockets and reconcile reconnects | Verified | 99bdfd4 | TypeScript, production build, Vitest | See verification report for live and fixture coverage |
| L | Identify every v1 mutation and bind paper orders to the selected instrument | Verified | cd48204 | TypeScript, production build, Vitest | See verification report for live and fixture coverage |
| M | Complete quote provenance, chart and depth navigation, instrument freshness and execution timestamps | Verified | 9c0e13a | TypeScript, production build, Vitest | See verification report for live and fixture coverage |
| N | Consolidate loading and recovery states for optional research data | Verified | aed1350 | TypeScript, production build, Vitest | See verification report for live and fixture coverage |
| O | Refine table access and keyboard focus on workspace changes | Verified | 13ad644 | TypeScript, production build, Vitest | See verification report for live and fixture coverage |
| P | Sort market quotes by symbol, price, spread and volume with missing values last | Verified | 48be739 | TypeScript, production build, Vitest | See verification report for live and fixture coverage |
| Q | Compare both depth ladders on the same scale and contextualize resting queues | Verified | 27c4290 | TypeScript, production build, Vitest | See verification report for live and fixture coverage |
| R | Filter execution history by aggressor and show latest fills first | Verified | 8b75e33 | TypeScript, production build, Vitest | See verification report for live and fixture coverage |
| S | Verify positive chart rendering and benchmark measurements in Chrome | Verified | 2492332 | TypeScript, production build, Vitest | See verification report for live and fixture coverage |
| T | Read run status, measured analytics and reproducibility metadata by run ID | Verified | d8b8bd2 | TypeScript, production build, Vitest | See verification report for live and fixture coverage |
| U | Expose instrument catalog metadata in the workstation | Verified | f696fca | TypeScript, production build, Vitest | See verification report for live and fixture coverage |
| V | Expose mark provenance, fees and measured event ordering counters | Verified | 233795e | TypeScript, production build, Vitest | See verification report for live and fixture coverage |
| W | Consolidate style overrides and verify final bundle | Verified | 098338b | TypeScript, build, 23 tests, 68 Chrome layouts | — |
| X | Expose historical candle values to keyboard and screen-reader users | Verified | 7a04e99 | Chrome canvas and source table, 23 tests | Live bars unavailable |
| Y | Keep recovery requests and stale quote indicators consistent across failed refreshes | Verified | b8c3def | In-flight gap regression, 24 tests, build | — |
| Z | Final verification and handoff | Verified | 0f03609 (tested application) | 31 tests, typecheck, build, Chrome sweep and screenshot review | See final verification report |
| AA | Resume paused strategies using the v1 lifecycle command | Verified | 990ee00 | Paused-strategy component test, typecheck, build | Positive lifecycle responses exercised with fixture |
| AB | Keep ticket drafts scoped to gateway units and instrument | Verified | 8e68fff | Invalid legacy price, keyboard focus, accessible description, build | — |
| AC | Reject malformed successful responses before displaying command acknowledgments | Verified | e36d8dd | 204, unreadable success and rejected-command regressions | — |
| AD | Reject partial optional snapshots before rendering ledger, strategies or telemetry | Verified | e5ae448 | Malformed optional resource regression, build | — |
| AE | Navigate workspaces using local URLs and browser history | Verified | 50331ef | Direct URL and Back regression, Chrome sweep | Unknown destinations open Overview |
| AF | Exercise disconnected and legacy states in the production browser | Verified | 673223e | Chrome forced v1 absence, offline commands and recovered v1 | Failure modes simulated |
| AG | Preserve service severity and display reported feed health by exchange | Verified | 30de8ca | Legacy feed quality test, typecheck, build | Exchange summaries reflect watched instruments |
| AH | Search the resting queue by order ID without disturbing FIFO order | Verified | a6905a3 | Filtered queue cancellation regression, build | — |
| AI | Preserve sub-unit measurements in benchmark and risk displays | Verified | d6046ac | Fractional elapsed-time regression, build | — |
| AJ | Recover malformed provider inventories and keep source metadata scoped | Verified | 7442a59 | Malformed-to-ready retry and credential visibility test | Live provider inventory unavailable |
| AK | Distinguish unavailable research prerequisites from an indexed empty catalog | Verified | 38eebfb | Missing dataset API and successful retry regression | Positive catalogs exercised with fixtures |
| AL | Keep queued backtest identifiers available for run inspection across workspace changes | Verified | b674102 | Two-run queue and older-run inspection regression | Latest 50 queued in this page session |
| AM | Check narrow layouts with long identifiers and large monetary values | Verified | dd0ef21 | Long symbol and large quote checks at 320px, full Chrome sweep | Stress values injected only in QA tab |
| AN | Align warning, acknowledgment and focus colors and verify text contrast | Verified | 0f03609 | Four text contrast pairs and final Chrome sweep | Coverage is limited to tested pairs |

The final application revision is `0f03609`. Milestone Z records that tested revision. The documentation-only handoff follows it in Git. Verification scope, commands and remaining service dependencies are in [the implementation report](../UI_REDESIGN_IMPLEMENTATION.md).
