import { useRef, useState } from "react";
import { Button, Card, Notice } from "@mib/ui";
import { invoke } from "../transport";
import { decodeRecipe, type Recipe } from "./recipeModel";
import { FindingNextRun } from "./FindingNextRun";
import { RecipeScience } from "./RecipeScience";
import { RecipePairing } from "./RecipePairing";
import { AnnotationGroups } from "./AnnotationGroups";
import { mintExperimentId, type PlanDraft } from "./planModel";
import { decodeSavedPlan, decodeWorkspaceDocument } from "./savedPlanModel";

export const PENDING_SAVE_KEY = "mib.workspace.pending-save.v1";
const recovered = () => { try { return sessionStorage.getItem(PENDING_SAVE_KEY); } catch { return null; } };
type WorkspaceDocument = { schema_version: 1; plan: PlanDraft; recipe?: Recipe; title?: string; archived?: boolean;
  sources?: Record<string, {path:string; sha256:string; size_bytes:string}>; [key:string]: unknown };
type Snapshot = { workspace_id: string; revision: string; document: WorkspaceDocument };
type Listed = {workspace_id:string; revision:string; title:string; archived:boolean;kind?:string};
/** Production metadata client. No automatic reads/writes, acquisition actions or localStorage fallback. */
export function SavedPlan({ draft, controlHeld, onLoad, onOpenSource, runObservation }: { runObservation?: Record<string,unknown>; draft: PlanDraft; controlHeld: boolean; onLoad?: (plan: PlanDraft) => boolean; onOpenSource?: (path:string)=>Promise<void> }) {
  const [id, setId] = useState(() => { try { return JSON.parse(recovered() ?? "null")?.workspace_id ?? draft.experimentId; } catch { return draft.experimentId; } });
  const [library,setLibrary] = useState<Listed[]>([]);
  const [listCursor,setListCursor] = useState("");
  const [annotationLayer,setAnnotationLayer] = useState("condition");
  const [title,setTitle] = useState("");
  const [manifest,setManifest] = useState<unknown>(null);
  const [importManifest,setImportManifest] = useState<unknown>(null);
  const [importId,setImportId] = useState(() => mintExperimentId());
  const [jobId,setJobId] = useState("");
  const [profileBase,setProfileBase] = useState("");
  const [methodResolution,setMethodResolution] = useState<{status:string;reason?:string;provider?:string}|null>(null);
  const [sourceId,setSourceId] = useState("");
  const [sourcePath,setSourcePath] = useState("");
  const [snapshot, setSnapshot] = useState<Snapshot | null>(null);
  const [message, setMessage] = useState("");
  const [busy, setBusy] = useState(false);
  const [retry, setRetry] = useState<string | null>(recovered);
  const [recipe, setRecipe] = useState<Recipe | null>(null);
  const [history, setHistory] = useState<string[]>([]);
  const [target, setTarget] = useState("");
  const editRecipe = (next: Recipe) => {setMethodResolution(null);setRecipe({...next, id: mintExperimentId(), revision: "1",
    lineage: {...next.lineage, recipe: snapshot?.document.recipe ? {id:snapshot.document.recipe.id, revision:snapshot.document.recipe.revision} : null}});};
  const pending = useRef(false);
  const clearPending = () => { sessionStorage.removeItem(PENDING_SAVE_KEY); setRetry(null); };
  const request = async (write: boolean, payload: string) => {
    if (pending.current || (write && !controlHeld)) return;
    pending.current = true; setBusy(true); setMessage("");
    try {
      if (write) { sessionStorage.setItem(PENDING_SAVE_KEY, payload); setRetry(payload); }
      const reply = await invoke<unknown>(write ? "workspace_write" : "workspace_read", { request: payload });
      if (!reply || typeof reply !== "object" || !("status" in reply)) throw new Error("Invalid metadata reply");
      const r = reply as Record<string, unknown>;
      if (r.status !== "ok") {
        // Storage failures may have committed. Keep the exact idempotent command for reconciliation.
        if (write && ["conflict", "invalid", "not_found", "unavailable"].includes(String(r.status))) clearPending();
        setSnapshot(null);
        setMessage(`${String(r.status)}: ${typeof r.message === "string" ? r.message : "Workspace was not read or saved"}. Reopen before editing an existing revision.`);
        return;
      }
      const q = JSON.parse(payload);
      if(q.action === "resolve_finding" || q.action === "verify_next_run") {
        if(!r.evidence || typeof r.evidence!=="object") throw new Error("Invalid finding evidence reply");
        setMessage("Native finding, source, analysis and method references verified at this check. Scientific compatibility remains unknown; nothing applied or started."); return;
      }
      if(q.action === "resolve_recipe") {
        if(r.workspace_id!==q.workspace_id || r.revision!==q.revision || !r.method || typeof r.method!=="object") throw new Error("Invalid recipe resolution reply");
        setMethodResolution(r.method as {status:string;reason?:string;provider?:string}); setMessage("Method lookup complete; scientific compatibility is separate."); return;
      }
      if (q.action === "list") {
        if (!Array.isArray(r.workspaces)) throw new Error("Invalid workspace listing");
        const rows=r.workspaces as Listed[];
        if (!rows.every(x => typeof x.workspace_id === "string" && typeof x.revision === "string" && typeof x.title === "string" && typeof x.archived === "boolean")) throw new Error("Invalid workspace listing");
        setLibrary(q.after ? prev => [...prev,...rows] : rows); setListCursor(rows.length===100 ? rows[rows.length-1].workspace_id : ""); return;
      }
      if (q.action === "export" || q.action === "export_status") {
        if (r.state!=="complete" || r.workspace_id!==q.workspace_id || r.job_id!==q.job_id || !r.manifest) throw new Error("Invalid export reply");
        setManifest(typeof r.manifest_json==="string"?r.manifest_json:r.manifest); setJobId(q.job_id); if(write) clearPending();
        setMessage(`Export ${q.job_id} materialized for immutable revision ${r.revision}. Download separately; source files and method configurations are not embedded. Unresolved analysis references: ${Array.isArray((r.manifest as {payload?:{unresolved_references?:unknown[]}})?.payload?.unresolved_references) ? (r.manifest as {payload:{unresolved_references:unknown[]}}).payload.unresolved_references.length : 0}.`); return;
      }
      if (q.action === "verify_source") {
        if(r.workspace_id!==q.workspace_id || r.revision!==q.revision || r.source_id!==q.source_id || r.verified!==true) throw new Error("Invalid source verification reply");
        setMessage(`Source ${q.source_id}: bytes match the pinned fingerprint at this check.`); return;
      }
      if (q.action === "history") {
        if (!Array.isArray(r.revisions) || !r.revisions.every(v => typeof v === "string" && /^[1-9][0-9]*$/.test(v))) throw new Error("Invalid history reply");
        setHistory(q.after ? prev => [...prev,...r.revisions as string[]] : r.revisions as string[]); return;
      }
      if (r.workspace_id !== q.workspace_id || typeof r.revision !== "string" || !/^[1-9][0-9]*$/.test(r.revision)) throw new Error("Invalid workspace identity in reply");
      if(typeof r.document_json==="string")r.document=JSON.parse(r.document_json);
      const decodedDocument = decodeWorkspaceDocument(r.document);
      const plan = decodedDocument.plan;
      const savedRecipe = (r.document as Record<string, unknown>).recipe;
      const decodedRecipe = savedRecipe === undefined ? null : decodeRecipe(savedRecipe);
      setId(q.workspace_id); setTitle(typeof (r.document as WorkspaceDocument).title === "string" ? (r.document as WorkspaceDocument).title! : "");
      setAnnotationLayer(typeof (decodedDocument.annotation_display as {layer?:unknown})?.layer === "string" ? (decodedDocument.annotation_display as {layer:string}).layer : "condition");
      setMethodResolution(null); setRecipe(decodedRecipe);
      setSnapshot({ workspace_id: r.workspace_id as string, revision: r.revision, document: { ...decodedDocument, schema_version: 1, plan, ...(decodedRecipe ? { recipe: decodedRecipe } : {}) } });
      if (write) clearPending();
      setMessage(`${write ? "Saved" : "Opened"} revision ${r.revision}. This is a recorded plan, not proof of acquisition or scientific compatibility.`);
    } catch (e) {
      setSnapshot(null);
      if (write) setRetry(payload);
      setMessage(`${String(e)}${write ? ". Save outcome unknown; retry the same save to reconcile." : ""}`);
    } finally { pending.current = false; setBusy(false); }
  };
  const save = () => {
    const document = { ...snapshot?.document, schema_version: 1, title, annotation_display:{layer:annotationLayer}, plan: draft, ...(runObservation ? {run_observation:{recorded_at:new Date().toISOString(),kind:"client-observation/1",...structuredClone(runObservation)}} : {}), ...(recipe ? {recipe} : {}) };
    try { decodeWorkspaceDocument(document); if (recipe) decodeRecipe(recipe); } catch (e) { setMessage(String(e)); return; }
    void request(true, JSON.stringify({ action: "commit", workspace_id: id, expected_revision: snapshot?.revision ?? null,
      command_id: mintExperimentId(), document }));
  };
  return <Card title="Saved workspace plan" data-testid="saved-plan">
    <p>Save the sample sheet and declared operator to the app metadata database. Reopen to inspect annotations. Opening only inspects. Load as new experiment explicitly replaces the editable sheet with a fresh experiment identity; nothing is applied or started.</p>
    <details><summary>Workspace library</summary>
      <Button disabled={busy || !!retry} onClick={() => void request(false,JSON.stringify({action:"list",limit:100}))}>List workspaces</Button>
      {listCursor && <Button disabled={busy || !!retry} onClick={() => void request(false,JSON.stringify({action:"list",limit:100,after:listCursor}))}>More workspaces</Button>}
      <ul>{library.filter(w=>!w.kind||w.kind==="workspace").map(w=><li key={w.workspace_id}><Button disabled={busy || !!retry} onClick={() => {setHistory([]);setManifest(null);void request(false,JSON.stringify({action:"read",workspace_id:w.workspace_id}));}}>{w.title || w.workspace_id} · revision {w.revision}{w.archived ? " · archived" : ""}</Button></li>)}</ul>
      <Button disabled={busy || !!retry} onClick={()=>{setSnapshot(null);setId(mintExperimentId());setTitle("");setRecipe(null);setHistory([]);setManifest(null);setMessage("New workspace identity. The active experiment was not changed.");}}>New workspace</Button>
    </details>
    <label>Workspace title <input aria-label="Workspace title" value={title} disabled={busy || !!retry || !controlHeld} onChange={e=>setTitle(e.target.value)}/></label>
    <label>Workspace ID <input aria-label="Saved workspace ID" value={id} disabled={busy || !!retry}
      onChange={e => { setId(e.target.value); setSnapshot(null); setRecipe(null); setHistory([]); setTarget(""); setMessage(""); }} /></label>
    <Button disabled={busy || !!retry || !id.trim()} onClick={() => void request(false, JSON.stringify({ action: "read", workspace_id: id }))}>Open saved plan</Button>
    <Button disabled={busy || !!retry || !controlHeld || !id.trim()} onClick={save}>{snapshot ? "Save new revision" : "Save new workspace"}</Button>
    <Button disabled={busy || !!retry || !id.trim()} onClick={() => void request(false, JSON.stringify({ action: "history", workspace_id: id, limit: 1000 }))}>Read revision history</Button>
    {history.length > 0 && <><label>Historical revision <select aria-label="Historical revision" value={target} onChange={e => setTarget(e.target.value)}><option value="">Choose revision</option>{history.map(r => <option key={r} value={r}>{r}</option>)}</select></label>
      <p>{history.length} revisions loaded. Restore appends a revision; it never deletes history.</p>
      {history.length % 1000 === 0 && <Button disabled={busy || !!retry} onClick={() => void request(false,JSON.stringify({action:"history",workspace_id:id,limit:1000,after:history[history.length-1]}))}>More revisions</Button>}
      <Button disabled={busy || !!retry || !target || !snapshot || !controlHeld} onClick={() => void request(true, JSON.stringify({ action: "restore", workspace_id: id, expected_revision: snapshot!.revision, target_revision: target, command_id: mintExperimentId() }))}>Restore selected revision</Button></>}
    {snapshot && onLoad && <Button disabled={busy || !!retry || !controlHeld} onClick={() => setMessage(onLoad(snapshot.document.plan) ? "Loaded as a new experiment. No settings applied or acquisition started." : "Cannot load while the current plan is running, pending, or locked. Make a new plan first.")}>Load as new experiment</Button>}
    {snapshot && <Button disabled={busy || !!retry || !controlHeld} onClick={()=>void request(true,JSON.stringify({action:"commit",workspace_id:id,expected_revision:snapshot.revision,command_id:mintExperimentId(),document:{...snapshot.document,archived:!snapshot.document.archived}}))}>{snapshot.document.archived ? "Unarchive workspace" : "Archive workspace"}</Button>}
    <details><summary>Export, import and source links</summary>
      <p>Export pins the opened revision and includes all saved definitions, findings and links. External source bytes are not embedded. Referenced analysis revisions must already be imported or created through the native analysis provider before importing this workspace; workspace import cannot create scientific authority records. A completed export job means the manifest is durable, not that your browser has saved the download.</p>
      <Button disabled={busy || !!retry || !snapshot || !controlHeld} onClick={()=>void request(true,JSON.stringify({action:"export",workspace_id:id,revision:snapshot!.revision,job_id:mintExperimentId()}))}>Prepare workspace export</Button>
      <label>Export job ID <input aria-label="Export job ID" value={jobId} onChange={e=>setJobId(e.target.value)}/></label>
      <Button disabled={busy || !!retry || !jobId || !id} onClick={()=>void request(false,JSON.stringify({action:"export_status",workspace_id:id,job_id:jobId}))}>Recover export job</Button>
      {manifest!==null && <Button onClick={()=>{const url=URL.createObjectURL(new Blob([typeof manifest==="string"?manifest:JSON.stringify(manifest,null,2)],{type:"application/json"}));const a=document.createElement("a");a.href=url;a.download=`workspace-${jobId}.json`;a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);}}>Download workspace manifest</Button>}
      <label>Import manifest <input aria-label="Import manifest" type="file" accept="application/json,.json" disabled={busy || !!retry} onChange={async e=>{setImportManifest(null);const file=e.target.files?.[0];if(!file)return;try{if(file.size>1100000)throw new Error("Import exceeds size limit");const text=await file.text();const data=JSON.parse(text);decodeSavedPlan(data.payload.document);setImportManifest(text);setMessage("Manifest selected; backend will verify its digest before creating a new workspace.");}catch(error){setMessage(String(error));}}}/></label>
      <label>Imported workspace ID <input aria-label="Imported workspace ID" value={importId} disabled={busy || !!retry} onChange={e=>setImportId(e.target.value)}/></label>
      <Button disabled={busy || !!retry || !controlHeld || !importManifest || !importId} onClick={()=>void request(true,JSON.stringify({action:"import",workspace_id:importId,command_id:mintExperimentId(),manifest:importManifest}))}>Import as new workspace</Button>
      <p>Source paths are on the backend host. Verification reads regular files only (up to 2 GiB, 10-second hashing limit), never writes them. Relink requires identical bytes, not just a matching name.</p>
      <label>Source ID <input aria-label="Source ID" value={sourceId} disabled={busy || !!retry} onChange={e=>setSourceId(e.target.value)}/></label>
      <label>Source file path <input aria-label="Source file path" value={sourcePath} disabled={busy || !!retry} onChange={e=>setSourcePath(e.target.value)}/></label>
      <Button disabled={busy || !!retry || !controlHeld || !snapshot || !sourceId || !sourcePath} onClick={()=>void request(true,JSON.stringify({action:snapshot?.document.sources?.[sourceId]?"relink":"attach_source",workspace_id:id,expected_revision:snapshot!.revision,command_id:mintExperimentId(),source_id:sourceId,path:sourcePath}))}>Verify and save source link</Button>
      <ul>{Object.entries(snapshot?.document.sources ?? {}).map(([key,source])=><li key={key}>{key} · {source.path} · SHA256 {source.sha256}<Button disabled={busy || !!retry} onClick={()=>void request(false,JSON.stringify({action:"verify_source",workspace_id:id,revision:snapshot!.revision,source_id:key}))}>Verify {key}</Button></li>)}</ul>
    </details>
    {retry && <Button disabled={busy || !controlHeld} onClick={() => void request(true, retry)}>Retry same save</Button>}
    <details><summary>Pinned method and analysis recipe</summary>
      <label>Local profile directory <input aria-label="Local profile directory" value={profileBase} disabled={busy || !!retry} onChange={e=>{setProfileBase(e.target.value);setMethodResolution(null);}}/></label>
      <Button disabled={busy || !!retry || !snapshot?.document.recipe} onClick={()=>void request(false,JSON.stringify({action:"resolve_recipe",workspace_id:id,revision:snapshot!.revision,profile_base:profileBase}))}>Resolve pinned method</Button>
      {methodResolution && <Notice tone="info" title={`Method ${methodResolution.status}`}>{methodResolution.provider ?? methodResolution.reason} · Resolution is not scientific compatibility or permission to apply.</Notice>}
      <p>Record immutable references supplied by your method/analysis provider. Save the references, then explicitly resolve the method and evaluate the pinned strategy below. Resolution does not establish scientific compatibility.</p>
      {!recipe && <Button disabled={busy || !!retry || !controlHeld} onClick={() => setRecipe({schema_version:1,id:mintExperimentId(),revision:"1",title:"Workspace recipe",question:"",method:{source:"local",id:"",revision:""},analysis_plan:{id:"",revision:""},lineage:{recipe:null,finding:null}})}>Add recipe references</Button>}
      {recipe && <>{(["title", "question"] as const).map(k => <label key={k}>{k}<input aria-label={`Recipe ${k}`} value={recipe[k]} disabled={busy || !!retry || !controlHeld} onChange={e => editRecipe({...recipe,[k]:e.target.value})}/></label>)}
      {(["method", "analysis_plan"] as const).map(k => <fieldset key={k}><legend>{k === "method" ? "Acquisition method" : "Analysis plan"}</legend>{(["id", "revision"] as const).map(f => <label key={f}>{f}<input aria-label={`${k} ${f}`} value={recipe[k][f]} disabled={busy || !!retry || !controlHeld} onChange={e => editRecipe({...recipe,[k]:{...recipe[k],[f]:e.target.value}})}/></label>)}</fieldset>)}
      <label>Method source <select aria-label="Method source" value={recipe.method.source} disabled={busy || !!retry || !controlHeld} onChange={e => editRecipe({...recipe,method:{...recipe.method,source:e.target.value as "local" | "registry"}})}><option value="local">Local</option><option value="registry">Registry</option></select></label>
      <RecipePairing recipe={snapshot?.document.recipe?.id === recipe.id ? recipe : {...recipe, revision:null}} contextRevision={null} compatibility={null}/></>}
    </details>
    {snapshot?.document.recipe && <FindingNextRun key={`${snapshot.workspace_id}:${snapshot.revision}`} workspaceId={snapshot.workspace_id} revision={snapshot.revision} profileBase={profileBase} blocked={busy||!!retry} controlHeld={controlHeld} evidence={snapshot.document.next_run_evidence} request={request}/>}
    {snapshot?.document.recipe && <RecipeScience workspaceId={snapshot.workspace_id} revision={snapshot.revision} recipe={snapshot.document.recipe} sources={snapshot.document.sources??{}} profileBase={profileBase} controlHeld={controlHeld} blocked={busy||!!retry} onOpenSource={onOpenSource}
      onSaveFinding={finding=>void request(true,JSON.stringify({action:"commit",workspace_id:id,expected_revision:snapshot.revision,command_id:mintExperimentId(),document:{...snapshot.document,findings:[...(Array.isArray(snapshot.document.findings)?snapshot.document.findings:[]),finding]}}))}/>}
    {Array.isArray(snapshot?.document.findings)&&snapshot.document.findings.length>0&&<details><summary>Saved findings ({snapshot.document.findings.length})</summary><ul>{snapshot.document.findings.map((f,i)=><li key={i}>{typeof f==="object"&&f!==null?String((f as Record<string,unknown>).interpretation??(f as Record<string,unknown>).id??"Finding"):"Unsupported finding"}<details><summary>Immutable finding evidence</summary><pre>{JSON.stringify(f,null,2)}</pre></details></li>)}</ul></details>}
    {snapshot?.document.run_observation!==undefined&&<details><summary>Recorded run observations</summary><p>Historical client observations, not current instrument state. Opening or loading a plan never resumes this queue or reuses its run identity.</p><pre>{JSON.stringify(snapshot.document.run_observation,null,2)}</pre></details>}
    {message && <Notice tone="info" title="Workspace persistence">{message}</Notice>}
    {snapshot && <><p>Workspace {snapshot.workspace_id} · immutable revision {snapshot.revision}</p><AnnotationGroups draft={snapshot.document.plan} selectedLayer={annotationLayer} onSelectLayer={controlHeld && !busy && !retry ? setAnnotationLayer : undefined} /></>}
  </Card>;
}
