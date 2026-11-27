import { useState, type FormEvent } from "react";
import { fetchOptional } from "./api";
import { formatNumber } from "./format";

const workloads = ["BALANCED", "BUY_HEAVY", "SELL_HEAVY", "AGGRESSIVE_MATCHING", "CANCEL_HEAVY",
  "CONCENTRATED_LEVELS", "WIDE_DISTRIBUTION", "BURSTY_ARRIVALS", "HIGH_ACTIVE_ORDERS",
  "MULTI_INSTRUMENT_50", "MARKET_DATA_BURST", "SLOW_CONSUMER", "ALL"];

interface Measurement {
  profile_name: string;
  implementation: string;
  orders_processed: number;
  throughput_ops_per_sec: number;
  p95_latency_ns: number;
  p99_latency_ns: number;
}

export function Benchmarks({ connected }: { connected: boolean }) {
  const [profile, setProfile] = useState("BALANCED");
  const [count, setCount] = useState("");
  const [baseline, setBaseline] = useState(true);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [results, setResults] = useState<Measurement[] | null>(null);
  async function run(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    const orders = Number(count);
    if (!connected || busy || !Number.isSafeInteger(orders) || orders < 1 || orders > 10000) return;
    setBusy(true);
    setError("");
    setResults(null);
    try {
      const response = await fetchOptional<{ results: Measurement[] }>("/api/benchmark", {
        method: "POST", body: JSON.stringify({ profile, order_count: orders, compare_baseline: baseline }),
      });
      if (!Array.isArray(response.results)) throw new Error("Gateway returned no result set.");
      setResults(response.results);
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : "Benchmark request failed.");
    } finally {
      setBusy(false);
    }
  }
  return <>
    <section className="panel">
      <form className="ticket-form benchmark-form" onSubmit={run}>
        <label>Workload<select value={profile} onChange={e => setProfile(e.target.value)}>
          {workloads.map(id => <option value={id} key={id}>{id.replaceAll("_", " ").toLowerCase()}</option>)}
        </select></label>
        <label>Order count<input type="number" min="1" max="10000" required value={count} onChange={e => setCount(e.target.value)} /></label>
        <label>Comparison<select value={String(baseline)} onChange={e => setBaseline(e.target.value === "true")}>
          <option value="true">Include baseline</option><option value="false">Engine only</option>
        </select></label>
        <button className="primary-button" disabled={!connected || busy}>{busy ? "Running…" : "Run benchmark"}</button>
      </form>
    </section>
    <section className="panel" aria-busy={busy}>
      <div className="section-heading"><h2>Measured results</h2></div>
      {error ? <p role="alert">{error}</p> : results?.length ? <div className="table-scroll" tabIndex={0} role="region" aria-label="Benchmark measurements">
        <table><thead><tr><th>Workload</th><th>Implementation</th><th>Orders</th><th>Ops/s</th><th>p95 ns</th><th>p99 ns</th></tr></thead>
          <tbody>{results.map(row => <tr key={row.profile_name + row.implementation}>
            <td>{row.profile_name}</td><td>{row.implementation}</td><td>{formatNumber(row.orders_processed)}</td>
            <td>{formatNumber(row.throughput_ops_per_sec)}</td><td>{formatNumber(row.p95_latency_ns)}</td><td>{formatNumber(row.p99_latency_ns)}</td>
          </tr>)}</tbody></table>
      </div> : <p role="status">{busy ? "Measuring workload…" : results ? "The gateway returned no measurements." : "Run a workload to measure throughput and latency."}</p>}
    </section>
  </>;
}
