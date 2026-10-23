import { AnalysisOverlay } from "./AnalysisOverlay";
import { AnalysisLibrary } from "./AnalysisLibrary";
import { AnalysisEmbedding } from "./AnalysisEmbedding";
import { AnalysisReport } from "./AnalysisReport";
import { AnalysisPlot, type PlotArtifact } from "./AnalysisPlot";
import { PopulationThumbnail } from "./PopulationThumbnail";
import { useEffect, useRef, useState } from "react";
import type { PopulationGraph } from "./PopulationAnalysis";

export type AnalysisCall = (request: Record<string, unknown>) => Promise<Record<string, any>>;
const pendingKey = "mib.analysis.pending-command.v1";
/** Backend revisions are authoritative; local storage holds only an unresolved command receipt. */
export function AnalysisWorkspace({ path, disabled, graphText, feature, read, write, job, live = false, onLoadGraph }: {
  path: string; disabled: boolean; graphText: string; feature: string; read: AnalysisCall; write: AnalysisCall; job: AnalysisCall; live?: boolean; onLoadGraph?: (graph: PopulationGraph) => void;
}) {
  const [displayDigits, setDisplayDigits] = useState(4);
  const formatStat = (value: number | null) => value == null ? "—" : value.toPrecision(displayDigits);
  const [strategyFeature, setStrategyFeature] = useState(feature);
  useEffect(() => setStrategyFeature(feature), [feature]);
  const [transformText, setTransformText] = useState("[]");
  const [templateId, setTemplateId] = useState("");
  const [templateRevision, setTemplateRevision] = useState("");
  const [overrides, setOverrides] = useState("[]");
  const [comparisonText, setComparisonText] = useState(JSON.stringify({conditions:["before","after"],design:"independent",weighting:"equal-experimental-unit",endpoint:{feature:"area",statistic:"mean",unit:"pixel2"},samples:[],descriptiveOnly:true},null,2));
  const [comparison, setComparison] = useState<Record<string, any> | null>(null);
  const [findingId, setFindingId] = useState("");
  const [findingNote, setFindingNote] = useState("");
  const [kind, setKind] = useState(live && !path ? "live-run" : "stored");
  const [retainedId, setRetainedId] = useState("");
  const [revision, setRevision] = useState("");
  const [method, setMethod] = useState<Record<string, unknown> | null>(null);
  const [application, setApplication] = useState<Record<string, unknown> | null>(null);
  const selection = useRef("");
  const [qcEnabled, setQcEnabled] = useState(false);
  const [qcPolicy, setQcPolicy] = useState("good-only");
  const [qcText, setQcText] = useState("[]");
  const [qcActor, setQcActor] = useState("");
  const [qcReason, setQcReason] = useState("");
  const [plotEnabled, setPlotEnabled] = useState(false);
  const [plotText, setPlotText] = useState(JSON.stringify({kind:"histogram",feature:"area",minimum:0,maximum:200,bins:40,normalization:"count",layers:[{population:"area-gate",displayOffset:0}]},null,2));
  const [plot, setPlot] = useState<PlotArtifact | null>(null);
  const [report, setReport] = useState<Record<string, any> | null>(null);
  const [manifestText, setManifestText] = useState("");
  const [gallery, setGallery] = useState<Record<string, any> | null>(null);
  const [restoreRevision, setRestoreRevision] = useState("");
  const [history, setHistory] = useState<string[]>([]);
  const [message, setMessage] = useState("");
  const [result, setResult] = useState<Record<string, any> | null>(null);
  const [jobX, setJobX] = useState("area"), [jobY, setJobY] = useState("deformability");
  const [embeddingEnabled, setEmbeddingEnabled] = useState(false);
  const [embeddingText, setEmbeddingText] = useState(JSON.stringify({engine:"sklearn-tsne-1.9.1",features:["area","deformability"],seed:42,perplexity:5,maxIter:500,maxPoints:1000},null,2));
  const [vertices, setVertices] = useState("[[0,0],[1000,0],[1000,1],[0,1]]");
  const [jobId, setJobId] = useState("");
  const [jobResult, setJobResult] = useState<Record<string, any> | null>(null);
  const [busy, setBusy] = useState(false);
  const generation = useRef(0);
  useEffect(() => { ++generation.current; setPlot(null); setReport(null); setGallery(null); setResult(null); setMessage(""); }, [path, kind, retainedId, graphText, feature]);
  useEffect(() => {
    let id: unknown; try { id = JSON.parse(graphText).id; } catch { return; }
    if (typeof id !== "string") return;
    const key = JSON.stringify([path, kind, retainedId, id]);
    if (key !== selection.current) {
      selection.current = key; setRevision(""); setHistory([]); setMethod(null); setApplication(null);
    }
  }, [path, kind, retainedId, graphText]);
  const source = () => kind === "retained" ? {kind, id: retainedId} : kind.startsWith("live-") ? { kind } : { kind, path };
  const graph = (): PopulationGraph => JSON.parse(graphText);
  const checked = (reply: Record<string, any>) => {
    if (reply.status !== "ok") throw new Error(`${reply.status}: ${reply.message || "Request refused"}`);
    return reply;
  };
  async function run(operation: (token: number) => Promise<void>) {
    const token = generation.current; setBusy(true); setMessage("");
    try { await operation(token); }
    catch (e) { if (token === generation.current) setMessage(String(e)); }
    finally { setBusy(false); }
  }
  function download(filename: string, text: string, type: string) {
    const url = URL.createObjectURL(new Blob([text], {type}));
    const anchor = document.createElement("a"); anchor.href = url; anchor.download = filename;
    anchor.click(); URL.revokeObjectURL(url);
  }
  async function reconcile(command: Record<string, unknown>, token: number) {
    const reply = checked(await write(command));
    // Clear only this exact journal entry, never an unrelated pending command.
    if (window.localStorage.getItem(pendingKey) === JSON.stringify(command)) window.localStorage.removeItem(pendingKey);
    if (token !== generation.current) return;
    if (command.action === "retainSource") {
      setRetainedId(reply.descriptor.id); setKind("retained");
    } else if (command.action === "saveFinding") {
      setComparison(reply.document); setMessage(`Immutable finding ${reply.findingId}, revision 1 saved.`);
    } else { setRevision(reply.revision); setMessage(`Backend revision ${reply.revision} saved. Source identity is pinned.`); }
  }
  return <section aria-label="Source-bound analysis workspace">
    <h4>Source-bound backend strategies</h4>
    <p>Strategies are immutable revisions in the application metadata database. Saving does not alter a recording or apply a hardware method. Stored-file fingerprints are limited to 64 MiB; larger files are explicitly refused.</p>
    <fieldset disabled={disabled || busy}>
      <label>Analysis source <select aria-label="Strategy source" value={kind} onChange={e => setKind(e.target.value)}>
        <option value="retained">Retained immutable snapshot</option><option value="stored">Stored observations</option><option value="run-archive">Archived E run events</option>
        {live && <><option value="live-run">Current E run</option><option value="live-epoch">Current E epoch</option></>}
      </select></label>
      <label>Retained snapshot ID<input aria-label="Retained snapshot ID" value={retainedId} onChange={e => setRetainedId(e.target.value)} /></label>
      <button onClick={() => void run(async token => {
        if (window.localStorage.getItem(pendingKey)) throw new Error("Reconcile the pending command first.");
        const command = {action:"retainSource",source:source(),commandId:crypto.randomUUID()};
        window.localStorage.setItem(pendingKey,JSON.stringify(command)); await reconcile(command,token);
      })}>Retain current snapshot</button>
      <label>Exact backend revision <input aria-label="Strategy revision" value={revision} onChange={e => { ++generation.current; setRevision(e.target.value); setGallery(null); setPlot(null); setReport(null); setResult(null); }} placeholder="Blank creates a new plan" /></label>
      <button onClick={() => void run(async token => {
        if (window.localStorage.getItem(pendingKey)) throw new Error("Reconcile the pending command before creating another revision.");
        const g = graph();
        const snapshot = checked(await read({ action: "snapshot", source: source() })).source;
        if (token !== generation.current) return;
        const binding = Object.fromEntries(["fingerprint", "sourceKey", "parameters", "scope"].map(k => [k, snapshot[k]]));
        const command = { action: "save", planId: g.id, expectedRevision: revision || null,
          commandId: crypto.randomUUID(), graph: g, method, ...(application ? {application} : {}), feature: strategyFeature, transforms: JSON.parse(transformText), source: source(), sourceBinding: binding, ...(plotEnabled ? {plot: JSON.parse(plotText)} : {}), ...(qcEnabled ? {qc:{policy:qcPolicy,decisions:JSON.parse(qcText)}} : {}) };
        // Journal before submission; a storage failure must prevent a write with no recovery handle.
        window.localStorage.setItem(pendingKey, JSON.stringify(command));
        await reconcile(command, token);
      })}>Save backend revision</button>
      <button disabled={!revision || !onLoadGraph} onClick={() => void run(async token => {
        const reply = checked(await read({action:"read",planId:graph().id,revision}));
        if (token !== generation.current) return;
        const doc = reply.document;
        setStrategyFeature(doc.feature); setTransformText(JSON.stringify(doc.transforms || [],null,2));
        setMethod(doc.method || null); setApplication(doc.application || null);
        setPlotEnabled(Boolean(doc.plot)); if (doc.plot) setPlotText(JSON.stringify(doc.plot,null,2));
        setQcEnabled(Boolean(doc.qc)); setQcPolicy(doc.qc?.policy || "good-only");
        setQcText(JSON.stringify(doc.qc?.decisions || [],null,2));
        onLoadGraph?.(doc.graph);
      })}>Load saved graph into editor</button>
      {application && <><p>This draft retains its pinned template application. Arbitrary edits require explicitly detaching the draft; saved history is unchanged.</p><button onClick={() => setApplication(null)}>Detach draft from template</button></>}
      <button onClick={() => void run(async token => {
        const pending = window.localStorage.getItem(pendingKey);
        if (!pending) throw new Error("No unresolved command");
        await reconcile(JSON.parse(pending), token);
      })}>Reconcile pending save</button>
      <button onClick={() => void run(async token => {
        const reply = checked(await read({ action: "history", planId: graph().id, limit: 100 }));
        if (token === generation.current) { setHistory(reply.revisions); setMessage("Select an exact revision. History is paged; the first 100 are shown."); }
      })}>Load backend history</button>
      <button disabled={!revision} onClick={() => void run(async token => {
        const reply = checked(await read({ action: "evaluate", planId: graph().id, revision, source: source() }));
        if (token === generation.current) { setResult(reply.evaluation); setMessage("Evaluated the saved graph, not unsaved editor changes."); }
      })}>Evaluate pinned revision</button>
      <label>Restore from revision<input aria-label="Restore strategy revision" value={restoreRevision} onChange={e => setRestoreRevision(e.target.value)} /></label>
      <button disabled={!revision || !restoreRevision} onClick={() => void run(async token => {
        if (window.localStorage.getItem(pendingKey)) throw new Error("Reconcile the pending command first.");
        const command = {action: "restore", planId: graph().id, expectedRevision: revision,
          commandId: crypto.randomUUID(), targetRevision: restoreRevision};
        window.localStorage.setItem(pendingKey, JSON.stringify(command));
        await reconcile(command, token);
      })}>Restore as new backend revision</button>
      <button disabled={!revision} onClick={() => void run(async token => {
        const reply = checked(await read({action: "gallery", planId: graph().id, revision, source: source(), offset: 0}));
        if (token === generation.current) setGallery(reply);
      })}>Load pinned gallery</button>
    </fieldset>
    <details><summary>Apply or resynchronize a pinned template</summary>
      <p>The destination is the graph editor’s plan ID. An exact upstream revision and explicit interval overrides create a new source-bound backend revision. Resynchronizing never moves a saved revision. Event-specific QC assertions are cleared, not copied between sources.</p>
      <fieldset disabled={disabled || busy}>
        <label>Template plan ID<input value={templateId} onChange={e=>setTemplateId(e.target.value)}/></label>
        <label>Template revision<input value={templateRevision} onChange={e=>setTemplateRevision(e.target.value)}/></label>
        <label>Interval overrides JSON<textarea value={overrides} onChange={e=>setOverrides(e.target.value)} placeholder='[{"node":"area-gate","minimum":100,"maximum":200}]'/></label>
        <button disabled={!templateId || !templateRevision} onClick={()=>void run(async token=>{
          if(window.localStorage.getItem(pendingKey)) throw new Error("Reconcile the pending command first.");
          const snapshot=checked(await read({action:"snapshot",source:source()})).source;
          if(token!==generation.current)return;
          const sourceBinding=Object.fromEntries(["fingerprint","sourceKey","parameters","scope"].map(k=>[k,snapshot[k]]));
          const command={action:"apply",planId:graph().id,expectedRevision:revision||null,commandId:crypto.randomUUID(),application:{planId:templateId,revision:templateRevision,overrides:JSON.parse(overrides)},source:source(),sourceBinding};
          window.localStorage.setItem(pendingKey,JSON.stringify(command)); await reconcile(command,token);
        })}>Apply pinned template to this source</button>
      </fieldset>
    </details>
    <details><summary>Experimental-unit comparisons and findings</summary>
      <p>Retain each source first. Specify exact plan/revision, retained source, condition and experimental-unit identity for each sample. Cell rows are not replicates. Paired designs require matching unit identities. Results are descriptive only; compatibility remains unknown.</p>
      <fieldset disabled={disabled || busy}>
        <label>Comparison specification<textarea aria-label="Comparison specification" rows={12} value={comparisonText} onChange={e=>{setComparisonText(e.target.value);setComparison(null);}}/></label>
        <button onClick={()=>void run(async token=>{
          const reply=checked(await read({action:"compare",spec:JSON.parse(comparisonText)}));
          if(token===generation.current)setComparison(reply);
        })}>Evaluate pinned comparison</button>
        <label>Immutable finding ID<input value={findingId} onChange={e=>setFindingId(e.target.value)}/></label>
        <label>Finding note<textarea value={findingNote} onChange={e=>setFindingNote(e.target.value)}/></label>
        <button disabled={!findingId} onClick={()=>void run(async token=>{
          if(window.localStorage.getItem(pendingKey))throw new Error("Reconcile the pending command first.");
          const command={action:"saveFinding",findingId,commandId:crypto.randomUUID(),spec:JSON.parse(comparisonText),note:findingNote};
          window.localStorage.setItem(pendingKey,JSON.stringify(command));await reconcile(command,token);
        })}>Save immutable finding</button>
        <button disabled={!findingId} onClick={()=>void run(async token=>{
          const reply=checked(await read({action:"readFinding",findingId,revision:"1"}));
          if(token===generation.current)setComparison(reply.document);
        })}>Load saved finding</button>
      </fieldset>
      {comparison && <><pre>{JSON.stringify(comparison,null,2)}</pre><button onClick={()=>download("analysis-finding.json",JSON.stringify(comparison,null,2),"application/json")}>Download comparison evidence</button></>}
    </details>
    <AnalysisOverlay read={read} write={write} disabled={disabled || busy}/>
    <AnalysisLibrary read={read} write={write} disabled={disabled || busy} onPrecision={setDisplayDigits}/>
    <AnalysisReport read={read} write={write} disabled={disabled || busy}/>
    <details><summary>Ordered analysis-parameter transforms</summary>
      <p>Derived parameters preserve raw measurements. Each stage computes (input − offset) / scale, then optionally asinh or log10. Scale must be positive; log10 of nonpositive values and overflow remain unknown. Outputs are dimensionless. Gate feature names and plot features may reference these outputs. Save a new backend revision to apply.</p>
      <label>Strategy reduction parameter<input aria-label="Strategy reduction parameter" value={strategyFeature} onChange={e=>setStrategyFeature(e.target.value)}/></label>
      <label>Transform stages JSON<textarea aria-label="Transform stages JSON" rows={8} value={transformText} onChange={e=>setTransformText(e.target.value)} placeholder='[{"input":"area","inputUnit":"pixel2","output":"scaled_area","kind":"asinh","scale":100,"offset":0}]'/></label>
      <p>Compensation and Logicle are not implemented here. No fluorescence channel, spillover control or calibration is inferred.</p>
    </details>
    <details><summary>Scientific QC assertions</summary>
      <p>Manual assertions are separate from processing validity. Unlabelled events remain unknown. “Good only” preserves unknown membership; these labels are not independent scientific qualification.</p>
      <label><input type="checkbox" checked={qcEnabled} disabled={busy} onChange={e=>setQcEnabled(e.target.checked)}/>Include QC assertions in the next saved revision</label>
      <label>QC policy<select value={qcPolicy} onChange={e=>setQcPolicy(e.target.value)}><option value="good-only">Good only; unknown remains unknown</option><option value="all">Do not filter; retain explicit QC counts</option></select></label>
      <label>Assertion author<input aria-label="QC assertion author" value={qcActor} onChange={e=>setQcActor(e.target.value)}/></label>
      <label>Scientific reason<input aria-label="QC assertion reason" value={qcReason} onChange={e=>setQcReason(e.target.value)}/></label>
      <label>Exact-key QC decisions<textarea aria-label="QC decisions JSON" rows={5} value={qcText} onChange={e=>setQcText(e.target.value)}/></label>
      <p>Use the pinned gallery buttons to stage assertions with the author/reason above, then save a new backend revision. A changed source cannot inherit foreign event labels.</p>
    </details>
    <details><summary>Saved histogram, scatter and overlay specification</summary>
      <p>Up to eight graph populations share fixed raw-value bins. Count, fraction or density normalization is explicit. Display offsets do not alter gates or statistics. Save a new backend revision to persist this specification.</p>
      <label><input type="checkbox" checked={plotEnabled} onChange={e=>setPlotEnabled(e.target.checked)} disabled={busy}/>Include PlotSpec in the next saved revision</label>
      <p>Scatter example: <code>{'{"kind":"scatter","x":"area","y":"deformability","seed":42,"maxPoints":1000,"layers":[{"population":"area-gate"}]}'}</code>. Its display sample never limits native population statistics.</p>
      <label>PlotSpec JSON<textarea aria-label="Analysis PlotSpec JSON" rows={10} value={plotText} onChange={e=>setPlotText(e.target.value)} /></label>
      <button disabled={busy || !revision} onClick={()=>void run(async token=>{
        const reply=checked(await read({action:"plot",planId:graph().id,revision,source:source()}));
        if(token===generation.current)setPlot(reply.plot);
      })}>Render saved PlotSpec</button>
      {plot && <AnalysisPlot artifact={plot}/>}
    </details>
    <details><summary>Population tables and portable strategy</summary>
      <p>Tables are computed from finite selected values in the saved parameter domain (raw or explicitly transformed). Quantiles use type 7; SD uses n−1; CV uses absolute mean. Null is unavailable, not zero. Exports pin the source and backend revision; they do not certify scientific compatibility.</p>
      <button disabled={busy || !revision} onClick={() => void run(async token => {
        const reply = checked(await read({action: "report", planId: graph().id, revision, source: source()}));
        if (token === generation.current) setReport(reply);
      })}>Build pinned population table</button>
      {report && <>
        <table><caption>Backend revision {report.report.revision}</caption><thead><tr><th>Population</th><th>Selected</th><th>Unknown</th><th>Mean</th><th>Median</th><th>P05</th><th>P95</th><th>Sample SD</th><th>CV %</th></tr></thead>
          <tbody>{report.report.table.map((r: Record<string, any>) => <tr key={r.population}>{["population","selected","unknown","mean","median","p05","p95","sampleStandardDeviation","cvPercent"].map(k=><td key={k}>{r[k] ?? "—"}</td>)}</tr>)}</tbody>
        </table>
        <p>Report content hash: {report.contentHash}</p>
        <button onClick={() => download("population-report.json",JSON.stringify(report,null,2),"application/json")}>Download evidence JSON</button>
        <button onClick={() => download("population-table.csv",report.csv,"text/csv")}>Download native CSV</button>
      </>}
      <button disabled={busy || !revision} onClick={() => void run(async () => {
        const reply = checked(await read({action: "manifest", planId: graph().id, revision}));
        const text = JSON.stringify({manifest:reply.manifest,contentHash:reply.contentHash},null,2);
        setManifestText(text); download("analysis-strategy.json",text,"application/json");
      })}>Export exact strategy manifest</button>
      <label>Portable manifest JSON<textarea aria-label="Strategy manifest JSON" rows={5} value={manifestText} onChange={e => setManifestText(e.target.value)} /></label>
      <button disabled={busy || disabled || !manifestText} onClick={() => void run(async token => {
        if (window.localStorage.getItem(pendingKey)) throw new Error("Reconcile the pending command first.");
        const imported = JSON.parse(manifestText);
        if (imported.manifest.planId !== graph().id) throw new Error("Load the manifest graph into the graph editor first; plan IDs must match.");
        const command = {action:"import",manifest:imported.manifest,contentHash:imported.contentHash,
          expectedRevision:revision || null,commandId:crypto.randomUUID(),source:source()};
        window.localStorage.setItem(pendingKey,JSON.stringify(command)); await reconcile(command,token);
      })}>Import against verified current source</button>
      <p>Import checks the complete content hash and source fingerprint. A copied recording can relink by identical bytes; a different recording is refused. The hash detects changes, but is not a signature.</p>
    </details>
    {gallery && <section aria-label="Pinned strategy gallery">
      <p>{gallery.selected} selected · plan {gallery.planId} revision {gallery.revision}. Source: {gallery.sourceFingerprint}</p>
      <div style={{display:"flex",flexWrap:"wrap"}}>{gallery.images.map((item: {key: string; pixels: number[] | null; reason?: string}) => <figure key={item.key}>
        {item.pixels ? <PopulationThumbnail pixels={item.pixels} /> : <span>{item.reason || "Image unavailable"}</span>}
        <figcaption style={{maxWidth:200,overflowWrap:"anywhere"}}>{item.key}</figcaption>
        {["good","bad","unknown"].map(status=><button key={status} disabled={busy || !qcActor.trim() || !qcReason.trim()} onClick={()=>{
          try {
            const decisions = JSON.parse(qcText) as Array<{key:string}>;
            if(!Array.isArray(decisions))throw new Error("QC decisions must be an array");
            setQcText(JSON.stringify([...decisions.filter(d=>d.key!==item.key),{key:item.key,status,actor:qcActor,reason:qcReason}],null,2));setQcEnabled(true);
            setMessage("QC assertion staged only. Save a new backend revision to persist it.");
          } catch(e){setMessage(String(e));}
        }}>{status}</button>)}
      </figure>)}</div>
      <button disabled={busy || gallery.offset === 0} onClick={() => void run(async token => {
        const reply = checked(await read({action:"gallery",planId:graph().id,revision,source:source(),offset:Math.max(0,gallery.offset-12)}));
        if (token === generation.current) setGallery(reply);
      })}>Previous pinned page</button>
      <button disabled={busy || gallery.offset+12 >= Number(gallery.selected)} onClick={() => void run(async token => {
        const reply = checked(await read({action:"gallery",planId:graph().id,revision,source:source(),offset:gallery.offset+12}));
        if (token === generation.current) setGallery(reply);
      })}>Next pinned page</button>
    </section>}
    {history.length > 0 && <p>Saved revisions: {history.map(r => <button key={r} disabled={busy} onClick={() => { ++generation.current; setRevision(r); setGallery(null); setPlot(null); setReport(null); setResult(null); }}>{r}</button>)}</p>}
    <p role="status">{message}</p>
    <details><summary>Cytoflow polygon job</summary>
      <p>Explicit x/y parameters, intersected with the pinned graph by native three-valued logic. The host freezes keyed inputs before scheduling; changing the source never retargets a running job. No hardware operation is performed.</p>
      <label>Job x parameter<input value={jobX} onChange={e=>setJobX(e.target.value)}/></label>
      <label>Job y parameter<input value={jobY} onChange={e=>setJobY(e.target.value)}/></label>
      <label><input type="checkbox" checked={embeddingEnabled} onChange={e=>setEmbeddingEnabled(e.target.checked)}/>Also compute seeded t-SNE on selected events with all requested features finite</label>
      {embeddingEnabled && <label>Embedding specification<textarea aria-label="Embedding specification" rows={7} value={embeddingText} onChange={e=>setEmbeddingText(e.target.value)}/></label>}
      <label>Polygon vertices<textarea aria-label="Cytoflow vertices" value={vertices} onChange={e => setVertices(e.target.value)} /></label>
      <button disabled={disabled || busy || !revision} onClick={() => void run(async token => {
        const existing = window.localStorage.getItem("mib.analysis.pending-job.v1");
        if (existing) throw new Error("Reconcile the pending job before another submission.");
        const snapshot = checked(await read({action: "snapshot", source: source()})).source;
        if (token !== generation.current) return;
        const submission = { commandId: crypto.randomUUID(), planId: graph().id, revision, source: source(),
          sourceFingerprint: snapshot.fingerprint, x: jobX, y: jobY, vertices: JSON.parse(vertices), ...(embeddingEnabled ? {embedding:JSON.parse(embeddingText)} : {}) };
        window.localStorage.setItem("mib.analysis.pending-job.v1", JSON.stringify(submission));
        const reply = await job({action: "submit", submission});
        setJobId(reply.jobId); setJobResult(reply);
      })}>Submit Cytoflow job</button>
      <button disabled={busy} onClick={() => void run(async () => {
        const text = window.localStorage.getItem("mib.analysis.pending-job.v1");
        if (!text) throw new Error("No pending job submission");
        const reply = await job({action: "reconcile", commandId: JSON.parse(text).commandId});
        setJobId(reply.jobId); setJobResult(reply);
        if (reply.status === "terminal") window.localStorage.removeItem("mib.analysis.pending-job.v1");
      })}>Reconcile job after reconnect</button>
      <button disabled={busy} onClick={() => void run(async () => {
        const text = window.localStorage.getItem("mib.analysis.pending-job.v1");
        if (!text) throw new Error("No pending job submission");
        const reply = await job({action:"submit",submission:JSON.parse(text)});
        setJobId(reply.jobId); setJobResult(reply);
      })}>Retry exact job submission</button>
      <button disabled={busy} onClick={() => { window.localStorage.removeItem("mib.analysis.pending-job.v1"); setMessage("Local recovery note cleared. Backend job/results were not cancelled or deleted; keep the Job ID to query them."); }}>Clear local job recovery note</button>
      <label>Job ID<input aria-label="Analysis job ID" value={jobId} onChange={e => setJobId(e.target.value)} /></label>
      <button disabled={busy || !jobId} onClick={() => void run(async () => { setJobResult(await job({action: "query", jobId})); })}>Refresh job</button>
      <button disabled={busy || !jobId} onClick={() => void run(async () => { setJobResult(await job({action: "cancel", jobId})); })}>Cancel job</button>
      {jobResult && <div role="status"><p>Job: {jobResult.status}</p>
        {jobResult.terminal?.failure && <p>{JSON.stringify(jobResult.terminal.failure)}</p>}
        {jobResult.terminal?.result && <>
          <p>{jobResult.terminal.result.rows.filter((r: {value: string}) => r.value === "true").length} selected; {jobResult.terminal.result.rows.filter((r: {value: string}) => r.value === "unknown").length} unknown.</p>
          <p>Source: {jobResult.terminal.result.revision.sourceFingerprint}</p>
          <p>Plan: {jobResult.terminal.result.revision.plan}</p>
          {jobResult.terminal.result.embedding && <AnalysisEmbedding artifact={jobResult.terminal.result.embedding}/>}
          <button onClick={()=>download("analysis-job-artifact.json",JSON.stringify(jobResult.terminal,null,2),"application/json")}>Download pinned job artifact</button>
        </>}
      </div>}
    </details>
    {result && <div>
      <p>{result.selected} selected · {result.excluded} excluded · {result.unknown} unknown. Denominator: {result.denominator ?? "unknown (incomplete event coverage)"}.</p>
      <p>Display precision {displayDigits} significant digits; exports retain native values.</p>
      <p>Mean: {formatStat(result.mean)} · Median: {formatStat(result.median)} {result.unit}</p>
      <p>QC: {result.qcPolicy}; good {result.qcCounts?.good}, bad {result.qcCounts?.bad}, unknown {result.qcCounts?.unknown}. {result.qcAuthority}</p>
      <p>Plan {result.planId} / revision {result.revision}</p>
      <code style={{ overflowWrap: "anywhere" }}>{result.sourceFingerprint}</code>
      <p>E event ordinals are not image indices. Live/archive image retention is unavailable until a provider supplies exact retained-asset locators.</p>
    </div>}
  </section>;
}
