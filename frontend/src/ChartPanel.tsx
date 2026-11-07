import { memo, useEffect, useRef, useState } from "react";
import {
  CandlestickSeries,
  ColorType,
  createChart,
  type CandlestickData,
  type IChartApi,
  type ISeriesApi,
  type Time,
} from "lightweight-charts";
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
  const chartRef = useRef<IChartApi | null>(null);
  const unavailableInstrumentsRef = useRef<Set<string>>(new Set());
  const [barCount, setBarCount] = useState(0);
  const [message, setMessage] = useState(
    effectiveMode === "legacy" || instrumentId === "SIM:LEGACY"
      ? LEGACY_NO_HISTORY_MESSAGE
      : "Waiting for historical bars from the local API.",
  );

  useEffect(() => {
    const container = containerRef.current;
    let chart: IChartApi | null = null;
    let series: ISeriesApi<"Candlestick"> | null = null;
    let resize: ResizeObserver | null = null;

    if (container && typeof ResizeObserver !== "undefined") {
      try {
        chart = createChart(container, {
          autoSize: true,
          layout: {
            background: { type: ColorType.Solid, color: "#111827" },
            textColor: "#94a3b8",
            fontFamily: "Inter, ui-sans-serif, system-ui",
          },
          grid: {
            vertLines: { color: "#202a3a" },
            horzLines: { color: "#202a3a" },
          },
          rightPriceScale: { borderColor: "#29364a" },
          timeScale: { borderColor: "#29364a", timeVisible: true },
          crosshair: { vertLine: { color: "#4b5d75" }, horzLine: { color: "#4b5d75" } },
        });
        series = chart.addSeries(CandlestickSeries, {
          upColor: "#10b981",
          downColor: "#f43f5e",
          borderUpColor: "#10b981",
          borderDownColor: "#f43f5e",
          wickUpColor: "#10b981",
          wickDownColor: "#f43f5e",
        });
        chartRef.current = chart;

        resize = new ResizeObserver((entries) => {
          const width = entries[0]?.contentRect.width ?? 0;
          if (width > 0 && chart) chart.applyOptions({ width });
        });
        resize.observe(container);
      } catch {
        chart = null;
        series = null;
      }
    }

    let active = true;
    if (effectiveMode === "legacy" || instrumentId === "SIM:LEGACY") {
      setBarCount(0);
      setMessage(LEGACY_NO_HISTORY_MESSAGE);
    } else if (!instrumentId) {
      setBarCount(0);
      setMessage("Select an instrument to load its history.");
    } else if (effectiveMode !== "v1") {
      setBarCount(0);
      setMessage("Waiting for historical bars from the local API.");
    } else if (unavailableInstrumentsRef.current.has(instrumentId)) {
      setBarCount(0);
      setMessage(LEGACY_NO_HISTORY_MESSAGE);
    } else {
      void fetchOptional<BarResponse>(
        "/api/v1/instruments/" + encodeURIComponent(instrumentId) + "/bars?interval=1m&limit=300",
      ).then((response) => {
        if (!active) return;
        const bars = (response.bars ?? []).map((bar) => ({
          time: Math.floor(bar.open_time_ns / 1_000_000_000) as Time,
          open: bar.open_micro / 10_000,
          high: bar.high_micro / 10_000,
          low: bar.low_micro / 10_000,
          close: bar.close_micro / 10_000,
        })) as CandlestickData<Time>[];
        series?.setData(bars);
        if (bars.length > 0) {
          chart?.timeScale().fitContent();
          setBarCount(bars.length);
          setMessage("");
        } else {
          setBarCount(0);
          setMessage("No bars are available for this instrument and interval.");
        }
      }).catch((error: unknown) => {
        if (!active) return;
        setBarCount(0);
        if (error instanceof ApiError && error.status === 404) {
          unavailableInstrumentsRef.current.add(instrumentId);
          setMessage(LEGACY_NO_HISTORY_MESSAGE);
        } else {
          setMessage("Historical bars endpoint is unavailable.");
        }
      });
    }

    return () => {
      active = false;
      resize?.disconnect();
      chart?.remove();
      chartRef.current = null;
    };
  }, [instrumentId, effectiveMode]);

  return (
    <section className="panel chart-panel">
      <div className="section-heading">
        <div>
          <div className="eyebrow">Price history</div>
          <h2>Candles and volume</h2>
        </div>
        <span className="quiet-label">1 minute · {barCount > 0 ? barCount + " bars" : "no bars"}</span>
      </div>
      <div className="chart-wrap">
        <div className="chart-canvas" ref={containerRef} aria-label="Instrument candlestick chart" />
        {barCount === 0 && <div className="chart-empty">{message}</div>}
      </div>
    </section>
  );
});
