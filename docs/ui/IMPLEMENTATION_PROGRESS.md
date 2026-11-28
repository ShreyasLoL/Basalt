# Workstation modernization

Baseline `70c7bcd` introduced quote hierarchy, skip-link positioning and formatter reuse. The prior session reported 16 passing tests, typecheck and build.

| ID | Implementation task | Status | Commit | Checks | Outstanding |
| --- | --- | --- | --- | --- | --- |
| A | Share responsive grid, surface and control tokens | Implemented | e11687c | TypeScript, production build, Vitest | Browser pass tracked in J |
| B | Make all workspace destinations accessible on small screens | Implemented | ae96d28 | TypeScript, production build, Vitest | Browser pass tracked in J |
| C | Tie Overview to instrument selection and expose account and activity context | Implemented | 11b8574 | TypeScript, production build, Vitest | Browser pass tracked in J |
| D | Label book columns, distinguish missing depth, identify executions and improve ticket gating | Implemented | 1a2f460 | TypeScript, production build, Vitest | Browser pass tracked in J |
| E | Handle chart loading, renderer failure and retry | Implemented | c456ef4 | TypeScript, production build, Vitest | Browser pass tracked in J |
| F | Improve strategy lifecycle, dataset selection and analytics grouping | Implemented | 4b64cb7 | TypeScript, production build, Vitest | Browser pass tracked in J |
| G | Implement measured benchmark workflow | Implemented | b04692b | TypeScript, production build, Vitest | Browser pass tracked in J |
| H | Distinguish flat portfolio, missing risk and provider loading states | Implemented | 98b07ee | TypeScript, production build, Vitest | Browser pass tracked in J |
| I | Scope replay feedback and distinguish REST-only monitoring | Implemented | 9269382 | TypeScript, production build, Vitest | Browser pass tracked in J |
| J | Run browser, responsive, keyboard and integration checks and record findings | Implemented | 1a37854 | TypeScript, production build, Vitest | Browser pass tracked in J |
| K | Ignore callbacks from retired sockets and reconcile reconnects | Implemented | 99bdfd4 | TypeScript, production build, Vitest | Browser pass tracked in J |
| L | Identify every v1 mutation and bind paper orders to the selected instrument | Implemented | cd48204 | TypeScript, production build, Vitest | Browser pass tracked in J |
| M | Complete quote provenance, chart and depth navigation, instrument freshness and execution timestamps | Implemented | 9c0e13a | TypeScript, production build, Vitest | Browser pass tracked in J |
| N | Consolidate loading and recovery states for optional research data | Implemented | aed1350 | TypeScript, production build, Vitest | Browser pass tracked in J |
| O | Refine table access and keyboard focus on workspace changes | Implemented | 13ad644 | TypeScript, production build, Vitest | Browser pass tracked in J |
| P | Sort market quotes by symbol, price, spread and volume with missing values last | Implemented | 48be739 | TypeScript, production build, Vitest | Browser pass tracked in J |
| Q | Compare both depth ladders on the same scale and contextualize resting queues | Implemented | 27c4290 | TypeScript, production build, Vitest | Browser pass tracked in J |
| R | Filter execution history by aggressor and show latest fills first | Implemented | 8b75e33 | TypeScript, production build, Vitest | Browser pass tracked in J |
| S | Verify positive chart rendering and benchmark measurements in Chrome | Implemented | 2492332 | TypeScript, production build, Vitest | Browser pass tracked in J |
| T | Read run status, measured analytics and reproducibility metadata by run ID | Implemented | d8b8bd2 | TypeScript, production build, Vitest | Browser pass tracked in J |
| U | Expose instrument catalog metadata in the workstation | Implemented | f696fca | TypeScript, production build, Vitest | Browser pass tracked in J |
| V | Expose mark provenance, fees and measured event ordering counters | Implemented | 233795e | TypeScript, production build, Vitest | Browser pass tracked in J |
| W | Consolidate style overrides and verify final bundle | Verified | 098338b | TypeScript, build, 23 tests, 68 Chrome layouts | — |
| X | Expose historical candle values to keyboard and screen-reader users | Verified | 7a04e99 | Chrome canvas and source table, 23 tests | Live bars unavailable |
| Y | Keep recovery requests and stale quote indicators consistent across failed refreshes | Not started | — | — | Test recovery during an outstanding poll |
| Z | Final verification and handoff | Not started | — | — | Record live and fixture coverage |
