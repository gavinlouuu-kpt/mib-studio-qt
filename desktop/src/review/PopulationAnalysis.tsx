import { useEffect, useRef, useState } from "react";
import { reviewBridge } from "./reviewBridge";

export interface PopulationResult {
  ok: boolean; message?: string; schema: "review-population/1";
  path: string; snapshot: string; feature: string; unit: string;
  valid: string; invalid: string; selected: string; excluded: string;
  unknown: string; measured: string; missing: string;
  minimum: number | null; maximum: number | null; mean: number | null; median: number | null;
  measurementSemantics: string;
}

/** Existing Review product surface; all science is evaluated by the native backend. */
export function PopulationAnalysis({ path, disabled, evaluate = reviewBridge.analyzePopulation }: {
  path: string; disabled: boolean; evaluate?: typeof reviewBridge.analyzePopulation;
}) {
  const [feature, setFeature] = useState("area");
  const [minimum, setMinimum] = useState("");
  const [maximum, setMaximum] = useState("");
  const [result, setResult] = useState<PopulationResult | null>(null);
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const sequence = useRef(0);
  useEffect(() => {
    ++sequence.current; setResult(null); setError(""); setBusy(false);
    return () => { ++sequence.current; };
  }, [path, disabled, feature, minimum, maximum]);
  async function analyze() {
    const token = ++sequence.current;
    setBusy(true); setError(""); setResult(null);
    try {
      const bound = (v: string) => {
        if (!v.trim()) return null;
        const n = Number(v);
        if (!Number.isFinite(n)) throw new Error("Enter finite bounds, or leave them blank.");
        return n;
      };
      const reply = await evaluate({ expectedPath: path, feature, minimum: bound(minimum), maximum: bound(maximum) });
      if (token !== sequence.current) return;
      if (!reply.ok) throw new Error(reply.message || "Analysis refused");
      if (reply.schema !== "review-population/1" || reply.path !== path || reply.feature !== feature)
        throw new Error("The source or analysis contract changed; run again.");
      setResult(reply);
    } catch (e) { if (token === sequence.current) setError(String(e)); }
    finally { if (token === sequence.current) setBusy(false); }
  }
  const format = (n: number | null) => n === null ? "—" : String(n);
  return <details className="population-analysis">
    <summary>Population analysis · stored observations</summary>
    <p>Inclusive interval across both valid and invalid observations. No scientific QC filter. Up to 10,000 observations; larger files are refused, never sampled.</p>
    <fieldset disabled={disabled || busy}>
      <legend>Interval gate</legend>
      <label>Measurement <select aria-label="Analysis measurement" value={feature} onChange={e => setFeature(e.target.value)}>
        <option value="area">Stored area (px²)</option><option value="deformability">Deformability</option>
      </select></label>{" "}
      <label>Minimum <input aria-label="Analysis minimum" type="number" step="any" placeholder="Unbounded" value={minimum} onChange={e => setMinimum(e.target.value)} /></label>{" "}
      <label>Maximum <input aria-label="Analysis maximum" type="number" step="any" placeholder="Unbounded" value={maximum} onChange={e => setMaximum(e.target.value)} /></label>{" "}
      <button onClick={() => void analyze()}>{busy ? "Analyzing…" : "Analyze population"}</button>
    </fieldset>
    {error && <p role="alert">{error}</p>}
    {result && <div role="status">
      <p>{result.selected} selected · {result.excluded} excluded · {result.unknown} unknown / {result.valid} valid + {result.invalid} invalid stored observations.</p>
      <table><caption>Selected population · {result.unit}</caption><thead><tr><th>Measured</th><th>Missing</th><th>Minimum</th><th>Maximum</th><th>Mean</th><th>Median</th></tr></thead>
        <tbody><tr><td>{result.measured}</td><td>{result.missing}</td><td>{format(result.minimum)}</td><td>{format(result.maximum)}</td><td>{format(result.mean)}</td><td>{format(result.median)}</td></tr></tbody></table>
      <p>{result.measurementSemantics}</p>
      <p>Local read-only evaluation. No verified content fingerprint or saved strategy; results are cleared when the source or gate changes.</p>
    </div>}
  </details>;
}
