# BASALT UI design alignment

## Source hierarchy

1. BASALT product behavior, the current API contract, and the security boundary define what the interface may claim or do.
2. `.agents/skills/anti-slop-rules.md` defines the explicit visual and interaction constraints for this redesign.
3. The user-provided Phase 3 and comprehensive UI redesign briefs define the requested scope and verification.
4. `DESIGN.md` supplies the existing near-black, graphite, fine-border, and restrained lavender identity. It is a marketing-canvas capture, so its brand cues are adapted to dense research workspaces rather than copied as page layouts.
5. Relevant accessibility, responsive, forms, data-table, and design-system references guide implementation details where they fit the product.

Conflicts resolve in favor of the actual product and user rules. BASALT is a local market research and paper-trading tool, not a promotional landing page. It uses real API state and explicit unavailable states; the redesign adds no demo figures, synthetic performance, broker path, or unsupported controls. Marketing-oriented imagery, decorative gradients, and scroll scenes are out of scope. No external design file is required by the repository.

## Current workspace audit

The audit is based on `frontend/src/App.tsx`, `ChartPanel.tsx`, `api.ts`, `store.ts`, `types.ts`, `docs/API_SPEC.md`, and `docs/FRONTEND.md`.

| Workspace | Current data and actions | Design direction | Truth boundary |
| --- | --- | --- | --- |
| Overview | Local health, selected market row, best bid/ask/spread, recent executions, navigation | Asymmetric hero quote summary with compact service status and a full-width execution table | All values come from the store; retain empty market and service states |
| Market Watch | Quotes, source, feed quality, filter, native instrument selection | Dense wide table as the primary surface, compact feed context, horizontally scrollable on narrow screens | No quotes or providers are fabricated; selection opens the actual instrument workspace |
| Instrument Workstation | Historical candles when available, internal book, trades, paper order ticket | Chart-led 7/5 split with book context and ticket beside it; execution history below chart | Legacy mode has no bars; chart and book remain separate data sources |
| Order Book | Internal CLOB bids/asks, resting orders and cancel actions | Paired depth panes above a full-width resting queue | External observed depth is explicitly disconnected on the current gateway |
| Strategy Lab | API-provided strategy lifecycle and start/pause commands | Lifecycle table first, compact built-in strategy explanation beside/below it | No strategy creation, signal generation, or performance claims without server support |
| Paper Trading | Paper order entry, legacy simulation reset, open orders, fills | Order entry and session controls in an unequal split; working queue and fills as separate tables | Simulation only; advanced order types require REST v1 |
| Portfolio & Risk | API cash/equity/P&L/positions, limits, rejections, kill switch | P&L strip, broad positions table, clear risk-status and rejection surfaces | Unknown differs from off; P&L color is semantic; API is the source of account values |
| Backtesting | API datasets and strategies; queue supported bar-approximation request | Focused configuration card beside honest run-history empty state | Only licensed API datasets and server strategies; no fabricated runs or metrics |
| Market Replay | Session ID and REST v1 play/pause/step/speed/seek commands | Compact session header plus transport and seek controls grouped by action | Legacy mode disables controls; v1 server may return 501 for unavailable routes |
| Analytics | Metric labels with blank values until a completed run exists | Grouped metric bands with one dominant return/risk section | Dashes remain until the API returns completed-run data |
| Data Sources | Optional provider status, quality, subscription count; provenance note | Status summary followed by provider table and provenance detail | Credentials never rendered; unavailable endpoint is distinguishable from empty provider list |
| System Monitor | Stream state, event sequence, bounded buffer, measured latency, activity | Runtime status band, telemetry table, event history | No inferred performance; telemetry only when API reports it |
| Settings | API mode, loopback bind default, paper-only boundary, health, credentials guidance | Compact posture tiles with security boundary visible | Read-only status view, not a writable settings form |

## Shared system decisions

- Use near-black and layered graphite surfaces, hairline borders, and sparse lavender accent from `DESIGN.md`; reserve red and green for price direction and P&L. Warnings use amber.
- Use a distinct restrained display face and neutral UI body face with local system fallbacks; use tabular numerals for changing values.
- Use a purpose-based asymmetric grid. Panels should earn their space from their task; dense tables remain dense and the main price chart gets the largest area.
- Keep tile radii in the 12–16px range, inputs near 6px, and status pills fully rounded. Avoid glows and broad shadows.
- Keep motion limited to short transform/opacity feedback. Support reduced motion and visible keyboard focus. Touch layouts must expose navigation and controls without hover.
- Reuse the existing native React/CSS setup; do not add an icon or component dependency for decorative value.

## Verification plan

Run the existing frontend typecheck, Vitest suite, and production build. Inspect the running local server at desktop, tablet, and phone widths, including all 13 workspaces and narrow-table behavior. Keep the server session already started for the user alive. Update `docs/FRONTEND.md` for the final interface and record the exact verification and limitations in `docs/UI_REDESIGN_IMPLEMENTATION.md`.
