import { memo, useEffect, useRef, useState } from "react";
import type { IChartApi, UTCTimestamp } from "lightweight-charts";
import { ApiError, fetchOptional } from "./api";
import { useWorkstation } from "./store";
import type { ApiMode } from "./types";

interface BarResponse {
  bars?: Array<{
    open_time_ns: number;
    open_micro: number;
    high_micro: number;
    low_micro: number;
    close_micro: number;
  }>;
}

interface ChartPanelProps {
  instrumentId: string | null;
  mode?: ApiMode;
}

const LEGACY_NO_HISTORY_MESSAGE =
  "Historical bars are unavailable — no history in legacy mode.";

export const ChartPanel = memo(function ChartPanel({ instrumentId, mode }: ChartPanelProps) {
  const storeMode = useWorkstation((state) => state.mode);
  const effectiveMode = mode ?? storeMode;
  const containerRef = useRef<HTMLDivElement | null>(null);
  const [attempt, setAttempt] = useState(0);
  const [count, setCount] = useState(0);
  const [feedback, setFeedback] = useState("");
  const [failed, setFailed] = useState(false);
  const available = effectiveMode === "v1" && !!instrumentId && instrumentId !== "SIM:LEGACY";
  const message = effectiveMode === "legacy" || instrumentId === "SIM:LEGACY" ? LEGACY_NO_HISTORY_MESSAGE
    : !instrumentId ? "Select an instrument to load its history."
    : effectiveMode !== "v1" ? "Historical bars require a REST v1 connection." : feedback;

  useEffect(() => {
    const controller = new AbortController();
    const container = containerRef.current;
    let chart: IChartApi | undefined;
    setCount(0);
    setFailed(false);
    setFeedback("Loading historical bars…");
    if (!available || !container) return;

    async function loadChart(element: HTMLDivElement) {
      const response = await fetchOptional<BarResponse>(
        "/api/v1/instruments/" + encodeURIComponent(instrumentId!) + "/bars?interval=1m&limit=300",
        { signal: controller.signal },
      );
      if (controller.signal.aborted) return;
      const bars = (response.bars ?? []).map(bar => ({
        time: Math.floor(bar.open_time_ns / 1_000_000_000) as UTCTimestamp,
        open: bar.open_micro / 10_000, high: bar.high_micro / 10_000,
        low: bar.low_micro / 10_000, close: bar.close_micro / 10_000,
      }));
      if (!bars.length) {
        setFeedback("No bars are available for this instrument and interval.");
        return;
      }
      const { createChart, CandlestickSeries, ColorType } = await import("lightweight-charts");
      if (controller.signal.aborted) return;
      const theme = getComputedStyle(element);
      const color = (name: string) => theme.getPropertyValue(name).trim();
      const grid = color("--chart-grid");
      chart = createChart(element, {
        autoSize: true,
        layout: { background: { type: ColorType.Solid, color: color("--chart-canvas") }, textColor: color("--muted"), fontFamily: theme.fontFamily },
        grid: { vertLines: { color: grid }, horzLines: { color: grid } },
        rightPriceScale: { borderColor: grid },
        timeScale: { borderColor: grid, timeVisible: true },
        crosshair: { vertLine: { color: color("--accent") }, horzLine: { color: color("--accent") } },
      });
      const up = color("--green"), down = color("--red");
      chart.addSeries(CandlestickSeries, {
        upColor: up, downColor: down, borderUpColor: up, borderDownColor: down,
        wickUpColor: up, wickDownColor: down, priceFormat: { type: "price", precision: 4, minMove: 0.0001 },
      }).setData(bars);
      chart.timeScale().fitContent();
      setCount(bars.length);
      setFeedback("");
    }
    void loadChart(container).catch((error: unknown) => {
      if (controller.signal.aborted) return;
      chart?.remove();
      chart = undefined;
      setFailed(true);
      setFeedback(error instanceof ApiError ? error.message : "Could not display historical bars.");
    });
    return () => { controller.abort(); chart?.remove(); };
  }, [instrumentId, available, attempt]);

  return (
    <section className="panel chart-panel">
      <div className="section-heading">
        <div>
          <div className="eyebrow">Price history</div>
          <h2>Candlestick price history</h2>
        </div>
        <span className="quiet-label">1 minute · {count > 0 ? count + " bars" : "no bars"}</span>
      </div>
      <div className="chart-wrap">
        <div className="chart-canvas" ref={containerRef} role="img" aria-label={"Candlestick price chart for " + (instrumentId ?? "no selected instrument")} />
        {count === 0 && <div className="chart-empty"><div role="status">{message}</div>{available && failed && <button className="subtle-button" onClick={() => setAttempt(value => value + 1)}>Retry history</button>}</div>}
      </div>
    </section>
  );
});
