import { PopulationThumbnail } from "./PopulationThumbnail";
import { useEffect, useRef, useState } from "react";
import type { PopulationGraph } from "./PopulationAnalysis";

export type AnalysisCall = (request: Record<string, unknown>) => Promise<Record<string, any>>;
const pendingKey = "mib.analysis.pending-command.v1";
/** Backend revisions are authoritative; local storage holds only an unresolved command receipt. */
export function AnalysisWorkspace({ path, disabled, graphText, feature, read, write, job, live = false }: {
  path: string; disabled: boolean; graphText: string; feature: string; read: AnalysisCall; write: AnalysisCall; job: AnalysisCall; live?: boolean;
}) {
  const [kind, setKind] = useState(live && !path ? "live-run" : "stored");
  const [revision, setRevision] = useState("");
  const [gallery, setGallery] = useState<Record<string, any> | null>(null);
  const [restoreRevision, setRestoreRevision] = useState("");
  const [history, setHistory] = useState<string[]>([]);
  const [message, setMessage] = useState("");
  const [result, setResult] = useState<Record<string, any> | null>(null);
  const [vertices, setVertices] = useState("[[0,0],[1000,0],[1000,1],[0,1]]");
  const [jobId, setJobId] = useState("");
  const [jobResult, setJobResult] = useState<Record<string, any> | null>(null);
  const [busy, setBusy] = useState(false);
  const generation = useRef(0);
  useEffect(() => { ++generation.current; setGallery(null); setResult(null); setRevision(""); setHistory([]); setMessage(""); }, [path, kind, graphText, feature]);
  const source = () => kind.startsWith("live-") ? { kind } : { kind, path };
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
  async function reconcile(command: Record<string, unknown>, token: number) {
    const reply = checked(await write(command));
    // Clear only this exact journal entry, never an unrelated pending command.
    if (window.localStorage.getItem(pendingKey) === JSON.stringify(command)) window.localStorage.removeItem(pendingKey);
    if (token !== generation.current) return;
    setRevision(reply.revision); setMessage(`Backend revision ${reply.revision} saved. Source identity is pinned.`);
  }
  return <section aria-label="Source-bound analysis workspace">
    <h4>Source-bound backend strategies</h4>
    <p>Strategies are immutable revisions in the application metadata database. Saving does not alter a recording or apply a hardware method. Stored-file fingerprints are limited to 64 MiB; larger files are explicitly refused.</p>
    <fieldset disabled={disabled || busy}>
      <label>Analysis source <select aria-label="Strategy source" value={kind} onChange={e => setKind(e.target.value)}>
        <option value="stored">Stored observations</option><option value="run-archive">Archived E run events</option>
        {live && <><option value="live-run">Current E run</option><option value="live-epoch">Current E epoch</option></>}
      </select></label>
      <label>Exact backend revision <input aria-label="Strategy revision" value={revision} onChange={e => { ++generation.current; setRevision(e.target.value); setGallery(null); setResult(null); }} placeholder="Blank creates a new plan" /></label>
      <button onClick={() => void run(async token => {
        if (window.localStorage.getItem(pendingKey)) throw new Error("Reconcile the pending command before creating another revision.");
        const g = graph();
        const snapshot = checked(await read({ action: "snapshot", source: source() })).source;
        if (token !== generation.current) return;
        const binding = Object.fromEntries(["fingerprint", "sourceKey", "parameters", "scope"].map(k => [k, snapshot[k]]));
        const command = { action: "save", planId: g.id, expectedRevision: revision || null,
          commandId: crypto.randomUUID(), graph: g, feature, source: source(), sourceBinding: binding };
        // Journal before submission; a storage failure must prevent a write with no recovery handle.
        window.localStorage.setItem(pendingKey, JSON.stringify(command));
        await reconcile(command, token);
      })}>Save backend revision</button>
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
    {gallery && <section aria-label="Pinned strategy gallery">
      <p>{gallery.selected} selected · plan {gallery.planId} revision {gallery.revision}. Source: {gallery.sourceFingerprint}</p>
      <div style={{display:"flex",flexWrap:"wrap"}}>{gallery.images.map((item: {key: string; pixels: number[] | null; reason?: string}) => <figure key={item.key}>
        {item.pixels ? <PopulationThumbnail pixels={item.pixels} /> : <span>{item.reason || "Image unavailable"}</span>}
        <figcaption style={{maxWidth:200,overflowWrap:"anywhere"}}>{item.key}</figcaption>
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
    {history.length > 0 && <p>Saved revisions: {history.map(r => <button key={r} disabled={busy} onClick={() => { ++generation.current; setRevision(r); setGallery(null); setResult(null); }}>{r}</button>)}</p>}
    <p role="status">{message}</p>
    <details><summary>Cytoflow polygon job</summary>
      <p>Area (px²) × deformability, intersected with the pinned graph by native three-valued logic. The host freezes keyed inputs before scheduling; changing the source never retargets a running job. No hardware operation is performed.</p>
      <label>Polygon vertices<textarea aria-label="Cytoflow vertices" value={vertices} onChange={e => setVertices(e.target.value)} /></label>
      <button disabled={disabled || busy || !revision} onClick={() => void run(async token => {
        const existing = window.localStorage.getItem("mib.analysis.pending-job.v1");
        if (existing) throw new Error("Reconcile the pending job before another submission.");
        const snapshot = checked(await read({action: "snapshot", source: source()})).source;
        if (token !== generation.current) return;
        const submission = { commandId: crypto.randomUUID(), planId: graph().id, revision, source: source(),
          sourceFingerprint: snapshot.fingerprint, x: "area", y: "deformability", vertices: JSON.parse(vertices) };
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
      <label>Job ID<input aria-label="Analysis job ID" value={jobId} onChange={e => setJobId(e.target.value)} /></label>
      <button disabled={busy || !jobId} onClick={() => void run(async () => { setJobResult(await job({action: "query", jobId})); })}>Refresh job</button>
      <button disabled={busy || !jobId} onClick={() => void run(async () => { setJobResult(await job({action: "cancel", jobId})); })}>Cancel job</button>
      {jobResult && <div role="status"><p>Job: {jobResult.status}</p>
        {jobResult.terminal?.failure && <p>{JSON.stringify(jobResult.terminal.failure)}</p>}
        {jobResult.terminal?.result && <>
          <p>{jobResult.terminal.result.rows.filter((r: {value: string}) => r.value === "true").length} selected; {jobResult.terminal.result.rows.filter((r: {value: string}) => r.value === "unknown").length} unknown.</p>
          <p>Source: {jobResult.terminal.result.revision.sourceFingerprint}</p>
          <p>Plan: {jobResult.terminal.result.revision.plan}</p>
        </>}
      </div>}
    </details>
    {result && <div>
      <p>{result.selected} selected · {result.excluded} excluded · {result.unknown} unknown. Denominator: {result.denominator ?? "unknown (incomplete event coverage)"}.</p>
      <p>Mean: {result.mean ?? "—"} · Median: {result.median ?? "—"} {result.unit}</p>
      <p>Plan {result.planId} / revision {result.revision}</p>
      <code style={{ overflowWrap: "anywhere" }}>{result.sourceFingerprint}</code>
      <p>E event ordinals are not image indices. Live/archive image retention is unavailable until a provider supplies exact retained-asset locators.</p>
    </div>}
  </section>;
}
