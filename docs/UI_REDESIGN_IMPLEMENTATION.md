# Workstation implementation and verification

The frontend continuation starts after `70c7bcd`. The final application revision tested on 2026-11-28 is `0f03609`. The [milestone tracker](ui/IMPLEMENTATION_PROGRESS.md) identifies the local commits.

## Acceptance results

| Check | Observed result |
| --- | --- |
| TypeScript and production compilation | Both passed. The production entry JavaScript was 92.87 kB gzip. The separate chart library was 53.09 kB gzip and loaded on demand. |
| Frontend tests | 31 passed across component, API and store suites. Tests cover request payloads, selection, sorting, cancellation, replay gating, optional-resource recovery and stream reconciliation. |
| Chrome navigation and layout | All 17 destinations opened at 1440, 768, 390 and 320 CSS pixels. All 68 page-width assertions passed with zero uncaught runtime exceptions. Wide data tables retained local scrolling. |
| Keyboard and contrast | Tab reached the skip link and Enter moved focus to workspace content. Component tests covered focus on navigation and invalid order input. Four representative foreground/background pairs passed 4.5:1 contrast. |
| Visual inspection | Desktop and phone screenshots of Overview, Portfolio & Risk and Market Replay were inspected during implementation. The checks identified a 320px picker overflow and overlapping rejection text. Both were corrected. |
| Gateway integration | The local C++ gateway supplied its synthetic research session, catalog and account data. A 37-order balanced benchmark returned three implementation measurements. |
| Browser fixtures | Three OHLC bars rendered on canvas and in the accessible candle table. A long symbol and large quote fit at 320px. Forced v1 unavailability exercised legacy replay gating. Simulated disconnection disabled paper submission, and restoring the gateway recovered v1. |

## Reproduce

Run the following from `frontend` with the locked dependencies installed.

```powershell
npm.cmd run typecheck
npm.cmd test -- --reporter=dot
npm.cmd run build
```

Serve the generated bundle through the local C++ gateway. Open a dedicated Chrome profile with a loopback debugging port and a blank tab. For a disposable gateway on port 18176 and Chrome on 19275, run the following from the repository root. The screenshot directory must exist.

```powershell
node frontend/scripts/workspace-smoke.mjs http://127.0.0.1:18176 19275 $env:TEMP --integration
```

The integration flag runs one bounded benchmark, intercepts selected API requests inside that tab for chart and failure-state tests, and restores normal requests afterward. Omitting the flag runs the layout, contrast and skip-link checks.

## Verification limits

The bundled gateway reports WebSocket upgrades unavailable. Stream reconnection, out-of-order quotes, buffering and gap recovery were exercised with frontend fixtures. Positive historical bars, strategy lifecycle, replay, backtest reports and provider inventories also used fixtures. Live success on those routes requires a gateway that implements them. Browser verification used desktop Chrome with emulated widths. Physical-device, screen-reader and other browser-engine behavior remain unverified.
