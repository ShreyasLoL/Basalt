import { cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import App from "./App";
import { ChartPanel } from "./ChartPanel";
import { useWorkstation } from "./store";
import { EMPTY_DASHBOARD } from "./types";

const legacyFixture = {
  market: {
    best_bid: 9_990,
    has_best_bid: true,
    best_ask: 10_010,
    has_best_ask: true,
    spread: 20,
    has_spread: true,
    last_trade_price: 10_000,
    has_last_trade_price: true,
    last_trade_quantity: 5,
    total_volume_traded: 25,
    active_order_count: 2,
  },
  book: {
    bids: [{ price: 9_990, total_volume: 10, order_count: 1 }],
    asks: [{ price: 10_010, total_volume: 12, order_count: 1 }],
    resting_orders: [],
  },
  trades: [],
  activity: [],
};

function jsonResponse(body: unknown, status = 200) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json" },
  });
}

function v1ApiFetch() {
  return vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
    const url = String(input);
    if (url === "/api/v1/health") return jsonResponse({ schema_version: 1, state: "OK", websocket_available: false });
    if (url === "/api/v1/market/watch") return jsonResponse({ instruments: [] });
    if (url === "/api/v1/strategies") return jsonResponse({ strategies: [{ id: "strategy-rsi", name: "RSI Mean Reversion", version: "1.0", state: "STOPPED", lastSignal: null, error: null }] });
    if (url === "/api/v1/datasets") return jsonResponse({ datasets: [{ id: "dataset-42", label: "Licensed sample", source: "HISTORICAL", license: "Permitted", range: "2025", hash: "sha256:example" }] });
    if (url === "/api/v1/portfolio/1") return jsonResponse({ accountId: "1", positions: [] });
    if (url === "/api/v1/risk/1") return jsonResponse({ killSwitch: false, limits: [], rejections: [] });
    if (url === "/api/v1/system/metrics") return jsonResponse({ stages: [], eventRate: null, queueDepth: null, replacements: null, dropped: null, duplicates: null, outOfOrder: null });
    if (url === "/api/v1/backtests" && init?.method === "POST") return jsonResponse({ run_id: "run-42" }, 202);
    if (url.startsWith("/api/v1/replay/") && url.endsWith("/commands")) return jsonResponse({ accepted: true });
    return jsonResponse({ error: "not_found" }, 404);
  });
}

describe("BASALT workstation", () => {
  beforeEach(() => {
    useWorkstation.setState({ ...EMPTY_DASHBOARD, optionalStatus: {} });
    vi.stubGlobal("fetch", vi.fn(async (input: RequestInfo | URL) => {
      const url = String(input);
      if (url.includes("/api/v1/health")) return jsonResponse({ error: "not_found" }, 404);
      if (url.includes("/api/state")) return jsonResponse(legacyFixture);
      return jsonResponse({ error: "not_found" }, 404);
    }));
  });

  afterEach(() => {
    cleanup();
    vi.useRealTimers();
    vi.unstubAllGlobals();
  });

  it("shows a real legacy simulation snapshot with its degraded provenance", async () => {
    render(<App />);
    await waitFor(() => expect(screen.getByText("BASALT-SIM")).toBeInTheDocument());
    expect(screen.getAllByText("DERIVED").length).toBeGreaterThan(0);
    expect(screen.getAllByText("DEGRADED").length).toBeGreaterThan(0);
    expect(screen.getByText("9,990 ticks")).toBeInTheDocument();
    expect(screen.queryByText("12,500")).not.toBeInTheDocument();
  });

  it("uses tick prices and limits the legacy order ticket to supported choices", async () => {
    render(<App />);
    await waitFor(() => expect(screen.getByText("BASALT-SIM")).toBeInTheDocument());
    fireEvent.click(screen.getByRole("button", { name: "Market Watch" }));
    expect(screen.getAllByText("9,990 ticks").length).toBeGreaterThan(0);
    fireEvent.click(screen.getByRole("button", { name: "Paper Trading" }));
    const price = screen.getByLabelText("Limit price (ticks)");
    expect(price).toHaveAttribute("placeholder", "Integer ticks");
    expect(price).toHaveValue("");
    expect(screen.getByRole("option", { name: "POST ONLY" })).toBeDisabled();
    expect(screen.getByRole("option", { name: "FOK" })).toBeDisabled();
  });

  it("shows the selected workspace and empty-data language", async () => {
    render(<App />);
    await waitFor(() => expect(screen.getByText("BASALT-SIM")).toBeInTheDocument());
    fireEvent.click(screen.getByRole("button", { name: "Portfolio & Risk" }));
    expect(screen.getByRole("heading", { name: "Portfolio & Risk" })).toBeInTheDocument();
    expect(screen.getByText("No account data")).toBeInTheDocument();
    expect(screen.getByText("No position snapshot")).toBeInTheDocument();
  });

  it("limits the market filter to Market Watch and lets users recover from no matches", async () => {
    render(<App />);
    await waitFor(() => expect(screen.getByText("BASALT-SIM")).toBeInTheDocument());
    expect(screen.queryByRole("textbox", { name: "Filter instruments" })).not.toBeInTheDocument();

    fireEvent.click(screen.getByRole("button", { name: "Market Watch" }));
    const filter = screen.getByRole("textbox", { name: "Filter instruments" });
    fireEvent.change(filter, { target: { value: "no-such-instrument" } });
    expect(screen.getByText("No instruments match this filter")).toBeInTheDocument();
    fireEvent.click(screen.getByRole("button", { name: "Clear filter" }));
    expect(screen.getByRole("button", { name: /BASALT-SIM/ })).toBeInTheDocument();
  });

  it("provides a skip link and native keyboard-operable instrument selection", async () => {
    render(<App />);
    await waitFor(() => expect(screen.getByText("BASALT-SIM")).toBeInTheDocument());
    expect(screen.getByRole("link", { name: "Skip to workspace content" })).toHaveAttribute("href", "#main-content");
    fireEvent.click(screen.getByRole("button", { name: "Market Watch" }));
    const instrument = screen.getByRole("button", { name: /BASALT-SIM/ });
    expect(instrument).toHaveAttribute("aria-pressed");
    fireEvent.click(instrument);
    expect(screen.getByRole("heading", { name: "Instrument Workstation" })).toBeInTheDocument();
  });

  it("submits the selected dataset and strategy with the supported backtest model", async () => {
    const fetchSpy = v1ApiFetch();
    vi.stubGlobal("fetch", fetchSpy);
    render(<App />);
    fireEvent.click(screen.getByRole("button", { name: "Backtesting" }));
    const dataset = await screen.findByLabelText("Dataset");
    await screen.findByRole("option", { name: /Licensed sample/ });
    fireEvent.change(dataset, { target: { value: "dataset-42" } });
    fireEvent.change(screen.getByLabelText("Strategy"), { target: { value: "strategy-rsi" } });
    fireEvent.click(screen.getByRole("button", { name: "Run backtest" }));

    await waitFor(() => expect(fetchSpy.mock.calls.some(([url, init]) => String(url) === "/api/v1/backtests" && init?.method === "POST")).toBe(true));
    const request = fetchSpy.mock.calls.find(([url, init]) => String(url) === "/api/v1/backtests" && init?.method === "POST");
    expect(JSON.parse(String(request?.[1]?.body))).toMatchObject({
      dataset_id: "dataset-42",
      strategy_id: "strategy-rsi",
      split: "chronological",
      execution_model: "BAR_APPROXIMATION_MODEL",
    });
  });

  it("requires an available v1 session and applies replay speed and event sequence on demand", async () => {
    const fetchSpy = v1ApiFetch();
    vi.stubGlobal("fetch", fetchSpy);
    render(<App />);
    fireEvent.click(screen.getByRole("button", { name: "Market Replay" }));
    const session = await screen.findByLabelText("Session ID");
    fireEvent.change(session, { target: { value: "session/a" } });
    expect(screen.getByText("SESSION ID ENTERED")).toBeInTheDocument();
    fireEvent.click(screen.getByRole("button", { name: "Play" }));
    await waitFor(() => expect(screen.getByText("Replay command accepted: play")).toBeInTheDocument());

    fireEvent.change(screen.getByLabelText("Speed"), { target: { value: "5x" } });
    fireEvent.click(screen.getByRole("button", { name: "Apply speed" }));
    await waitFor(() => expect(screen.getByText("Replay command accepted: speed")).toBeInTheDocument());
    fireEvent.change(screen.getByLabelText("Target event sequence"), { target: { value: "42" } });
    fireEvent.click(screen.getByRole("button", { name: "Seek to event" }));

    await waitFor(() => expect(fetchSpy.mock.calls.filter(([url, init]) => String(url).startsWith("/api/v1/replay/") && init?.method === "POST")).toHaveLength(3));
    const commands = fetchSpy.mock.calls
      .filter(([url, init]) => String(url).startsWith("/api/v1/replay/") && init?.method === "POST")
      .map(([url, init]) => ({ url: String(url), body: JSON.parse(String(init?.body)) }));
    expect(commands[0]).toMatchObject({ url: "/api/v1/replay/session%2Fa/commands", body: { action: "play" } });
    expect(commands[1]).toMatchObject({ body: { action: "speed", speed: "5x" } });
    expect(commands[2]).toMatchObject({ body: { action: "seek", position: 42 } });
  });

  it("keeps replay transport disabled in legacy mode", async () => {
    render(<App />);
    await waitFor(() => expect(useWorkstation.getState().mode).toBe("legacy"));
    fireEvent.click(screen.getByRole("button", { name: "Market Replay" }));
    expect(screen.getByRole("button", { name: "Play" })).toBeDisabled();
    expect(screen.getByText("Replay commands are unavailable until the REST v1 service is connected.")).toBeInTheDocument();
  });

  it("reports an inactive kill switch when the risk API explicitly says it is off", async () => {
    vi.stubGlobal("fetch", v1ApiFetch());
    render(<App />);
    fireEvent.click(screen.getByRole("button", { name: "Portfolio & Risk" }));
    expect(await screen.findByText("KILL SWITCH OFF")).toBeInTheDocument();
  });

  it("keeps analytics empty until a completed run exists", () => {
    render(<App />);
    fireEvent.click(screen.getByRole("button", { name: "Analytics" }));
    expect(screen.getByText("No completed run is selected. BASALT does not show placeholder returns or synthetic performance figures.")).toBeInTheDocument();
    expect(screen.getAllByText("—").length).toBeGreaterThan(0);
  });

  it("makes zero /api/v1 calls between 30s re-probe intervals in legacy mode", async () => {
    vi.useFakeTimers();
    const fetchSpy = vi.fn(async (input: RequestInfo | URL) => {
      const url = String(input);
      if (url.includes("/api/v1/health")) return jsonResponse({ error: "not_found" }, 404);
      if (url.includes("/api/state")) return jsonResponse(legacyFixture);
      return jsonResponse({ error: "not_found" }, 404);
    });
    vi.stubGlobal("fetch", fetchSpy);

    render(<App />);
    await vi.advanceTimersByTimeAsync(10);
    expect(useWorkstation.getState().mode).toBe("legacy");

    const v1CallsAfterInitialProbe = fetchSpy.mock.calls.filter(([input]) =>
      String(input).includes("/api/v1"),
    ).length;
    expect(v1CallsAfterInitialProbe).toBe(1);

    fetchSpy.mockClear();

    await vi.advanceTimersByTimeAsync(29_000);
    const v1CallsBetweenIntervals = fetchSpy.mock.calls.filter(([input]) =>
      String(input).includes("/api/v1"),
    ).length;
    const legacyCallsBetweenIntervals = fetchSpy.mock.calls.filter(([input]) =>
      String(input).includes("/api/state"),
    ).length;
    expect(v1CallsBetweenIntervals).toBe(0);
    expect(legacyCallsBetweenIntervals).toBeGreaterThanOrEqual(29);

    await vi.advanceTimersByTimeAsync(1_000);
    const v1CallsAfterReprobeWindow = fetchSpy.mock.calls.filter(([input]) =>
      String(input).includes("/api/v1/health"),
    ).length;
    expect(v1CallsAfterReprobeWindow).toBe(1);
  });

  it("shows a clear no-history message in ChartPanel in legacy mode without fetching /bars", () => {
    const fetchSpy = vi.fn(async () => jsonResponse({ error: "not_found" }, 404));
    vi.stubGlobal("fetch", fetchSpy);

    render(<ChartPanel instrumentId="SIM:LEGACY" mode="legacy" />);
    expect(screen.getByText(/no history in legacy mode/i)).toBeInTheDocument();
    expect(fetchSpy).not.toHaveBeenCalled();
  });
});
