import { useEffect, useState, type FormEvent } from "react";
import { fetchOptional } from "./api";

interface RunReport {
  status?: string;
  metrics?: { name: string; units: string; status: string; value: number | null }[];
  manifest?: { basalt_revision?: string; dataset?: { id?: string; sha256?: string }; result_hashes?: { output_sha256?: string } };
}

export function RunResults({ initialId, connected }: { initialId: string; connected: boolean }) {
  const [input, setInput] = useState(initialId);
  const [request, setRequest] = useState<{ id: string } | null>(null);
  const [report, setReport] = useState<RunReport | null>(null);
  const [error, setError] = useState("");
  const [loading, setLoading] = useState(false);
  useEffect(() => {
    setReport(null);
    setError("");
    setLoading(Boolean(connected && request));
    if (!request || !connected) return;
    const controller = new AbortController();
    void fetchOptional<RunReport>("/api/v1/backtests/" + encodeURIComponent(request.id), { signal: controller.signal })
      .then(result => {
        if (!controller.signal.aborted) setReport(result);
      }).catch((cause: unknown) => {
        if (!controller.signal.aborted) setError(cause instanceof Error ? cause.message : "Run lookup failed.");
      }).finally(() => {
        if (!controller.signal.aborted) setLoading(false);
      });
    return () => controller.abort();
  }, [request, connected]);
  function inspect(event: FormEvent) {
    event.preventDefault();
    if (input.trim() && connected) setRequest({ id: input.trim() });
  }
  const manifest = report?.manifest;
  return <section className="panel run-results">
    <div className="section-heading"><h2>Inspect a run</h2></div>
    <form className="seek-controls" onSubmit={inspect}>
      <label>Run ID<input required value={input} onChange={event => setInput(event.target.value)} /></label>
      <button className="subtle-button" disabled={!connected || loading || !input.trim()}>Load results</button>
    </form>
    {loading && <p role="status">Loading run results…</p>}
    {error && <p role="alert">{error}</p>}
    {report && <div>
      <p role="status">Run {request?.id} · {report.status ?? "Status unavailable"}</p>
      {manifest && <dl className="facts">
        <dt>Revision</dt><dd>{manifest.basalt_revision ?? "—"}</dd>
        <dt>Dataset</dt><dd>{manifest.dataset?.id ?? "—"}</dd>
        <dt>Dataset SHA-256</dt><dd>{manifest.dataset?.sha256 ?? "—"}</dd>
        <dt>Output SHA-256</dt><dd>{manifest.result_hashes?.output_sha256 ?? "—"}</dd>
      </dl>}
      {report.metrics?.length ? <div className="table-scroll" role="region" aria-label="Run metrics" tabIndex={0}>
        <table><thead><tr><th>Metric</th><th>Value</th><th>Units</th><th>Status</th></tr></thead>
        <tbody>{report.metrics.map(metric => <tr key={metric.name}><th scope="row">{metric.name}</th>
          <td>{metric.value ?? "—"}</td><td>{metric.units}</td><td>{metric.status}</td></tr>)}</tbody></table>
      </div> : <p>No metrics returned for this run.</p>}
    </div>}
  </section>;
}
