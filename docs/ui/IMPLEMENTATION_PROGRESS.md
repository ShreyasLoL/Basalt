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
| T | Read run status, measured analytics and reproducibility metadata by run ID | In progress | — | — | Live run endpoint unavailable |
| U | Expose instrument catalog metadata in the workstation | Not started | — | — | Verify live catalog without displaying provider keys |
| V | Expose mark provenance, fees and measured event ordering counters | Not started | — | — | Verify live portfolio and monitor |
| W | Consolidate style overrides and verify final bundle | Not started | — | — | Repeat responsive checks |
