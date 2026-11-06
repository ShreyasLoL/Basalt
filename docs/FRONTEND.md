# BASALT Institutional Workstation Frontend

## Stack and boundary

Target is React + TypeScript + Vite + Tailwind, Zustand for normalized UI state, and Lightweight Charts for licensed local chart presentation. Static assets are served by the local C++ gateway with the legacy web/index.html as fallback. The frontend calls only schema-versioned REST v1 and WebSocket v1. It contains no matching, risk, portfolio accounting or synthetic quote logic. Empty, stale, disconnected and unavailable values are shown as such; there are no fake charts, trades, benchmark outputs or success states.

## Workspaces

1. **Overview:** feed/system health, watchlist summary, portfolio headline and recent paper events.
2. **Market Watch:** symbol, exchange, LTP, bid/ask/spread, change, volume, session high/low, source badge and quality/stale age.
3. **Instrument Workstation:** candles and volume, 20-level available depth, time-and-sales, source/quality detail, paper order ticket.
4. **Order Book:** external observed depth and internal CLOB shown in separate panels; FIFO queue visible only for internal simulated book.
5. **Strategy Lab:** create/configure/validate/start/pause/stop, parameter schema, warm-up, signals, per-run metrics and failure reason.
6. **Paper Trading:** account selection, order/execution blotters, model selection, simulated fill disclosure and cancel/modify controls.
7. **Portfolio & Risk:** cash/position ledger, conservative marks, realized/unrealized P&L, fees, exposure gauges, limit utilization, risk rejection log and emergency kill switch.
8. **Backtesting:** dataset selection and license/provenance, chronology/splits, model/cost config, reproducibility manifest and 18-metric tearsheet.
9. **Market Replay:** play/pause/step/seek, 1x/5x/10x/MAX, sequence/time position, gap indicators and deterministic result hashes.
10. **Analytics:** equity/drawdown, return distribution, trade/fee/slippage breakdown and capacity assumptions.
11. **Data Sources:** adapters, source, license label, connectivity, age, subscription and event counters; never display credential values.
12. **System Monitor:** T0–T10 latency p50/p95/p99, event rates, queue depth/replacements/drops, duplicates/out-of-order/gaps, storage health and build metadata.
13. **Settings:** theme, layout, local config, limits and redacted provider status.

## Visual and interaction rules

Dark-first palette: background #090D16, panel #111827, positive/bid #10B981, negative/ask #F43F5E, warning/stale #F59E0B, text #E5E7EB. Use tabular numbers and monospace for sequence/price/quantity. Responsive grid supports laptop and narrow screens, keyboard focus is visible, color is never the only state cue, and confirm only destructive simulation resets where data would be lost. Keyboard shortcuts are discoverable and do not override browser/system shortcuts.

## State model and update rate

Zustand owns entity maps keyed by stable IDs and bounded ring buffers for time-series views. A WebSocket ingest worker validates frame schema and updates an external typed store. UI quote renders are throttled/coalesced (target at most 10–20 visual updates/sec per active view); chart series batch updates outside broad React render loops. Executions, order state changes, feed faults, risk events and sequence gaps are never silently dropped. On gap/reconnect, request a REST snapshot, reconcile sequence, then resume. Components expose loading, empty, stale, degraded, disconnected and error states.

## Design system workflow

Tokens are source-controlled and versioned. Figma MCP can inspect layouts/tokens only when available; Figma output is not runtime dependency. Maintain accessible color contrast, typography, density levels and component states in code. Avoid generated screens with hard-coded sample figures or a dependency on an authenticated design file.

## Build and serving acceptance

Frontend package lock is committed; scripts provide dev, typecheck, build and tests. Production build emits a static bundle into a known CMake/server asset directory; generated output is ignored unless release policy explicitly checks it in. Offline fallback remains visible if assets are absent. Validate routes with actual API fixtures and a browser smoke run; verify keyboard, responsive layout, stale badges, reconnect/gap, form validation and no secret leakage.
