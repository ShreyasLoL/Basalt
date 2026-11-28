# BASALT Institutional Workstation Frontend

## Stack and boundary

Target is React + TypeScript + Vite + Tailwind, Zustand for normalized UI state, and Lightweight Charts for licensed local chart presentation. Static assets are served by the local C++ gateway with the legacy web/index.html as fallback. The frontend uses same-origin REST v1, optional WebSocket v1, and the documented local compatibility routes for legacy sessions and measured benchmarks. It contains no matching, risk, portfolio accounting or synthetic quote logic. Empty, stale, disconnected and unavailable values are shown as such; there are no fake charts, trades, benchmark outputs or success states.

The current bundled HTTP gateway reports `websocket_available: false` because cpp-httplib has no upgrade hook. The client skips stream connection and reconnect attempts when that flag is false; REST v1 remains available.

## Workspaces

1. **Overview:** local service health, one market-watch summary, best bid/ask/spread, recent paper trades and navigation into related workspaces.
2. **Market Watch:** symbol, exchange, last/bid/ask/spread, volume, source and feed quality. The instrument filter is scoped to this workspace; selecting an instrument uses a native button inside the table row.
3. **Instrument Workstation:** candlestick price history when the local API provides bars, available book depth, recent executions and the paper order ticket. The chart does not claim to display volume.
4. **Order Book:** internal simulated CLOB depth and resting queue. External market depth is explicitly disconnected on the current server.
5. **Strategy Lab:** API-provided lifecycle state and start/pause commands. Creating or configuring strategies is not implemented in the current frontend/server contract.
6. **Paper Trading:** supported paper order entry, working order cancellation, execution blotter and legacy-only simulation reset controls.
7. **Portfolio & Risk:** API-provided cash, equity, P&L, positions, risk utilization, rejection log and v1 kill-switch control. Unknown risk state is distinct from an explicitly inactive kill switch.
8. **Backtesting:** dataset and strategy values come from the local API; the only displayed execution model is bar-based approximation. Empty API data stays empty and unsupported requests return the server error rather than fabricated runs or metrics.
9. **Market Replay:** session ID entry, play/pause/step, explicit speed application and event-sequence seek. Commands require REST v1 and are serialized while pending; the current server reports unimplemented routes with 501.
10. **Analytics:** inspect a stored run by ID, including reported metric values, units, statuses and manifest hashes. Backtesting links directly to runs queued in the current page session.
11. **Data Sources:** API-reported provider status when available; credentials are never rendered.
12. **System Monitor:** API-reported system metrics and audit events, with empty states when the service does not provide them.
13. **Settings:** current API mode, bind default, simulation boundary, health and credential-handling guidance. This is a posture view, not an editable settings form.

14. **Charts:** selected-instrument one-minute candles, lazy-loaded renderer, accessible OHLC table and retry for failed history requests.
15. **Orders:** searchable resting-order queue, remaining quantity, FIFO sequence and per-order cancellation.
16. **Executions:** selected-instrument paper fills, newest first, with aggressor filtering, event times and execution models.
17. **Benchmarks:** bounded gateway workloads, optional baseline comparison and measured throughput, elapsed time and latency percentiles.

Workspace URLs use the `workspace` query parameter and support browser Back and Forward. Desktop has grouped sidebar navigation. The compact shell uses a grouped native selector. Market Watch supports sorting and instrument ID search. Overview follows the selected instrument and shows exchange quote quality, account and activity context.

## Visual and interaction rules

Dark-first palette follows the root `DESIGN.md`: near-black canvas, layered graphite panels, fine hairlines and sparse lavender accents. Positive/bid, negative/ask, warning/stale and error states retain distinct semantic colors. Use tabular numbers and monospace for sequence/price/quantity. Responsive grids support laptop and narrow screens, keyboard focus is visible, color is never the only state cue, and confirm only destructive simulation resets where data would be lost. Keyboard shortcuts are discoverable and do not override browser/system shortcuts.

## State model and update rate

Zustand stores dashboard entities and bounded recent activity; `App` and memoized workspace panels subscribe through field selectors. `api.ts` validates and bounds WebSocket frame buffering before updating the store. Executions, order changes, feed faults, risk events and sequence gaps are retained; quote updates can be coalesced. When `mode === "v1"` and the stream is connected without a flagged gap, periodic REST refresh merges newer state instead of overwriting it. On a gap or reconnect, it requests a REST snapshot. In legacy mode, it re-probes v1 at most every 30 seconds or on manual refresh, makes no WebSocket or optional `/api/v1/*` requests, displays a compatibility banner, and `ChartPanel` shows a no-history message without polling `/bars`. Views preserve empty, stale, degraded, disconnected and error states.

## Design system workflow

Tokens are source-controlled and versioned. Figma MCP can inspect layouts/tokens only when available; Figma output is not runtime dependency. Maintain accessible color contrast, typography, density levels and component states in code. Avoid generated screens with hard-coded sample figures or a dependency on an authenticated design file.

## Build and serving acceptance

Frontend package lock is committed; scripts provide dev, typecheck, build and tests. Production build emits a static bundle into a known CMake/server asset directory; generated output is ignored unless release policy explicitly checks it in. Offline fallback remains visible if assets are absent. Validate routes with actual API fixtures and a browser smoke run; verify keyboard, responsive layout, stale badges, reconnect/gap, form validation and no secret leakage.

## Current verification

On 2026-11-28, TypeScript, all 31 Vitest tests and the production build passed. Chrome exercised 17 destinations at four viewport widths against the bundled gateway. The optional-resource client retries unavailable endpoints on a 30-second probe or a forced refresh. Chart code loads after historical bars arrive.

See [the implementation report](UI_REDESIGN_IMPLEMENTATION.md) for browser reproduction steps, service dependencies and fixture coverage. The [milestone tracker](ui/IMPLEMENTATION_PROGRESS.md) records the corresponding commits.
