import { cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import App from "./App";

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

describe("BASALT workstation", () => {
  beforeEach(() => {
    vi.stubGlobal("fetch", vi.fn(async (input: RequestInfo | URL) => {
      const url = String(input);
      if (url.includes("/api/v1/health")) return jsonResponse({ error: "not_found" }, 404);
      if (url.includes("/api/state")) return jsonResponse(legacyFixture);
      return jsonResponse({ error: "not_found" }, 404);
    }));
  });

  afterEach(() => {
    cleanup();
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

  it("keeps analytics empty until a completed run exists", () => {
    render(<App />);
    fireEvent.click(screen.getByRole("button", { name: "Analytics" }));
    expect(screen.getByText("No completed run is selected. BASALT does not show placeholder returns or synthetic performance figures.")).toBeInTheDocument();
    expect(screen.getAllByText("—").length).toBeGreaterThan(0);
  });
});
