import { PopulationGraphEditor } from "./PopulationGraphEditor";
import { PopulationThumbnail } from "./PopulationThumbnail";
import { useEffect, useRef, useState } from "react";
import { AnalysisWorkspace, type AnalysisCall } from "./AnalysisWorkspace";
import { reviewBridge } from "./reviewBridge";

export interface PopulationGraph {
  id: string; revision: string; output: string;
  nodes: Array<
    { id: string; parent: string; kind: "interval"; feature: string; minimum: number | null; maximum: number | null } |
    { id: string; parent: string; kind: "ellipse"; x: string; y: string; centerX:number; centerY:number; radiusX:number; radiusY:number } |
    { id:string; parent:string; kind:"quadrant"; x:string; y:string; thresholdX:number; thresholdY:number; quadrant:"lower-left"|"lower-right"|"upper-left"|"upper-right" } |
    { id: string; parent: string; kind: "and" | "or" | "not"; operands: string[] }
  >;
}
export interface PopulationRequest {
  expectedPath: string; feature: string; minimum: number | null; maximum: number | null; graph?: PopulationGraph; galleryOffset?: number;
}
const strategyKey = "mib.analysis.graph.recipe.v1";
export const exampleGraph: PopulationGraph = { id: "population-plan", revision: "1", output: "area-gate", nodes: [
  { id: "area-gate", parent: "", kind: "interval", feature: "area", minimum: 100, maximum: 110 },
] };

export interface PopulationResult {
  ok: boolean; message?: string; schema: "review-population/1";
  path: string; snapshot: string; feature: string; unit: string;
  valid: string; invalid: string; selected: string; excluded: string;
  unknown: string; measured: string; missing: string;
  minimum: number | null; maximum: number | null; mean: number | null; median: number | null;
  measurementSemantics: string;
  gallery?: Array<{ key: string; set: string; ordinal: string; pixels: number[] | null }>;
  galleryOffset?: number | null;
}

/** Existing Review product surface; all science is evaluated by the native backend. */
export function PopulationAnalysis({ path, disabled, evaluate = reviewBridge.analyzePopulation, analysisRead = reviewBridge.analysisRead, analysisWrite = reviewBridge.analysisWrite, analysisJob = reviewBridge.analysisJob, live = false }: {
  path: string; disabled: boolean; evaluate?: typeof reviewBridge.analyzePopulation; analysisRead?: AnalysisCall; analysisWrite?: AnalysisCall; analysisJob?: AnalysisCall; live?: boolean;
}) {
  const [graphEnabled, setGraphEnabled] = useState(false);
  const [graphText, setGraphText] = useState(JSON.stringify(exampleGraph, null, 2));
  const [saved, setSaved] = useState("");
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
  }, [path, disabled, feature, minimum, maximum, graphEnabled, graphText]);
  async function analyze(galleryOffset?: number) {
    const token = ++sequence.current;
    setBusy(true); setError(""); setResult(null);
    try {
      const bound = (v: string) => {
        if (!v.trim()) return null;
        const n = Number(v);
        if (!Number.isFinite(n)) throw new Error("Enter finite bounds, or leave them blank.");
        return n;
      };
      const request: PopulationRequest = { expectedPath: path, feature, minimum: bound(minimum), maximum: bound(maximum) };
      if (galleryOffset !== undefined) request.galleryOffset = galleryOffset;
      if (graphEnabled) request.graph = JSON.parse(graphText);
      const reply = await evaluate(request);
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
    <details><summary>Population graph recipe</summary>
      <p>Interval, AND, OR and NOT nodes are evaluated by the native backend. Parent IDs restrict populations. Unknown measurements remain unknown under NOT. The selected measurement above controls the reduction, not graph membership.</p>
      <label><input type="checkbox" checked={graphEnabled} disabled={busy || disabled} onChange={e => setGraphEnabled(e.target.checked)} />Use graph instead of interval membership</label>
      <PopulationGraphEditor text={graphText} onChange={value=>{setGraphText(value);setSaved("");}} disabled={busy||disabled}/>
      <label>Advanced graph JSON<textarea aria-label="Population graph JSON" rows={14} style={{ display: "block", width: "min(100%, 48rem)", fontFamily: "monospace" }} value={graphText} disabled={busy || disabled} onChange={e => { setGraphText(e.target.value); setSaved(""); }} /></label>
      <button disabled={busy} onClick={() => { try { JSON.parse(graphText); window.localStorage.setItem(strategyKey, graphText); setSaved("Recipe saved in this browser. Source and results are not saved."); } catch (e) { setError(String(e)); } }}>Save recipe</button>
      <button disabled={busy} onClick={() => { try { const text = window.localStorage.getItem(strategyKey); if (!text) throw new Error("No saved recipe in this browser"); JSON.parse(text); setGraphText(text); setGraphEnabled(true); setSaved("Recipe restored. Analyze to evaluate against the current source."); } catch (e) { setError(String(e)); } }}>Restore recipe</button>
      <p>{saved}</p>
      <p>Browser-local recipe only; not a verified source-bound workspace. Copy the JSON to transfer it. Backend validates all nodes, cycles and budgets before publishing a result.</p>
    </details>
    <AnalysisWorkspace path={path} disabled={disabled} graphText={graphText} feature={feature} read={analysisRead} write={analysisWrite} job={analysisJob} live={live} onLoadGraph={g=>{setGraphText(JSON.stringify(g,null,2));setGraphEnabled(true);}} />
    {error && <p role="alert">{error}</p>}
    {result && <div role="status">
      <p>{result.selected} selected · {result.excluded} excluded · {result.unknown} unknown / {result.valid} valid + {result.invalid} invalid stored observations.</p>
      <table><caption>Selected population · {result.unit}</caption><thead><tr><th>Measured</th><th>Missing</th><th>Minimum</th><th>Maximum</th><th>Mean</th><th>Median</th></tr></thead>
        <tbody><tr><td>{result.measured}</td><td>{result.missing}</td><td>{format(result.minimum)}</td><td>{format(result.maximum)}</td><td>{format(result.mean)}</td><td>{format(result.median)}</td></tr></tbody></table>
      <p>{result.measurementSemantics}</p>
      <section aria-label="Gated gallery">
        <h4>Selected observations</h4>
        <p>12 per page, in valid-set then invalid-set row order. Images are copied with the evaluated source; unavailable images do not remove observations. Loading a page runs a fresh evaluation.</p>
        <button disabled={busy || disabled} onClick={() => void analyze(0)}>Load gated gallery</button>
        {result.galleryOffset != null && <>
          <button disabled={busy || disabled || result.galleryOffset === 0} onClick={() => void analyze(Math.max(0, result.galleryOffset! - 12))}>Previous selected page</button>
          <button disabled={busy || disabled || result.galleryOffset + 12 >= Number(result.selected)} onClick={() => void analyze(result.galleryOffset! + 12)}>Next selected page</button>
          <div style={{ display: "flex", flexWrap: "wrap" }}>{result.gallery?.map(item => <figure key={item.key}>
            {item.pixels ? <PopulationThumbnail pixels={item.pixels} /> : <span>Image unavailable</span>}
            <figcaption>{item.set} · row {item.ordinal}</figcaption>
          </figure>)}</div>
        </>}
      </section>
      <p>Local read-only evaluation. No verified content fingerprint or saved result; results are cleared when the source or gate changes.</p>
    </div>}
  </details>;
}
