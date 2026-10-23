import { useRef, useState } from "react";
import { Button, Card, Notice } from "@mib/ui";
import { invoke } from "../transport";
import { decodeRecipe, type Recipe } from "./recipeModel";
import { RecipePairing } from "./RecipePairing";
import { AnnotationGroups } from "./AnnotationGroups";
import { mintExperimentId, type PlanDraft } from "./planModel";
import { decodeSavedPlan } from "./savedPlanModel";

export const PENDING_SAVE_KEY = "mib.workspace.pending-save.v1";
const recovered = () => { try { return sessionStorage.getItem(PENDING_SAVE_KEY); } catch { return null; } };
type Snapshot = { workspace_id: string; revision: string; document: { schema_version: 1; plan: PlanDraft; recipe?: Recipe } };
/** Production metadata client. No automatic reads/writes, acquisition actions or localStorage fallback. */
export function SavedPlan({ draft, controlHeld, onLoad }: { draft: PlanDraft; controlHeld: boolean; onLoad?: (plan: PlanDraft) => boolean }) {
  const [id, setId] = useState(() => { try { return JSON.parse(recovered() ?? "null")?.workspace_id ?? draft.experimentId; } catch { return draft.experimentId; } });
  const [snapshot, setSnapshot] = useState<Snapshot | null>(null);
  const [message, setMessage] = useState("");
  const [busy, setBusy] = useState(false);
  const [retry, setRetry] = useState<string | null>(recovered);
  const [recipe, setRecipe] = useState<Recipe | null>(null);
  const [history, setHistory] = useState<string[]>([]);
  const [target, setTarget] = useState("");
  const editRecipe = (next: Recipe) => setRecipe({...next, id: mintExperimentId(), revision: "1",
    lineage: {...next.lineage, recipe: snapshot?.document.recipe ? {id:snapshot.document.recipe.id, revision:snapshot.document.recipe.revision} : null}});
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
      if (q.action === "history") {
        if (!Array.isArray(r.revisions) || !r.revisions.every(v => typeof v === "string" && /^[1-9][0-9]*$/.test(v))) throw new Error("Invalid history reply");
        setHistory(r.revisions as string[]); return;
      }
      if (r.workspace_id !== q.workspace_id || typeof r.revision !== "string" || !/^[1-9][0-9]*$/.test(r.revision)) throw new Error("Invalid workspace identity in reply");
      const plan = decodeSavedPlan(r.document);
      const savedRecipe = (r.document as Record<string, unknown>).recipe;
      const decodedRecipe = savedRecipe === undefined ? null : decodeRecipe(savedRecipe);
      setRecipe(decodedRecipe);
      setSnapshot({ workspace_id: r.workspace_id as string, revision: r.revision, document: { schema_version: 1, plan, ...(decodedRecipe ? { recipe: decodedRecipe } : {}) } });
      if (write) clearPending();
      setMessage(`${write ? "Saved" : "Opened"} revision ${r.revision}. This is a recorded plan, not proof of acquisition or scientific compatibility.`);
    } catch (e) {
      setSnapshot(null);
      if (write) setRetry(payload);
      setMessage(`${String(e)}${write ? ". Save outcome unknown; retry the same save to reconcile." : ""}`);
    } finally { pending.current = false; setBusy(false); }
  };
  const save = () => {
    const document = { schema_version: 1, plan: draft, ...(recipe ? {recipe} : {}) };
    try { decodeSavedPlan(document); if (recipe) decodeRecipe(recipe); } catch (e) { setMessage(String(e)); return; }
    void request(true, JSON.stringify({ action: "commit", workspace_id: id, expected_revision: snapshot?.revision ?? null,
      command_id: mintExperimentId(), document }));
  };
  return <Card title="Saved workspace plan" data-testid="saved-plan">
    <p>Save the sample sheet and declared operator to the app metadata database. Reopen to inspect annotations. Opening only inspects. Load as new experiment explicitly replaces the editable sheet with a fresh experiment identity; nothing is applied or started.</p>
    <label>Workspace ID <input aria-label="Saved workspace ID" value={id} disabled={busy || !!retry}
      onChange={e => { setId(e.target.value); setSnapshot(null); setRecipe(null); setHistory([]); setTarget(""); setMessage(""); }} /></label>
    <Button disabled={busy || !!retry || !id.trim()} onClick={() => void request(false, JSON.stringify({ action: "read", workspace_id: id }))}>Open saved plan</Button>
    <Button disabled={busy || !!retry || !controlHeld || !id.trim()} onClick={save}>{snapshot ? "Save new revision" : "Save new workspace"}</Button>
    <Button disabled={busy || !!retry || !id.trim()} onClick={() => void request(false, JSON.stringify({ action: "history", workspace_id: id, limit: 1000 }))}>Read revision history</Button>
    {history.length > 0 && <><label>Historical revision <select aria-label="Historical revision" value={target} onChange={e => setTarget(e.target.value)}><option value="">Choose revision</option>{history.map(r => <option key={r} value={r}>{r}</option>)}</select></label>
      <p>First {history.length} revisions (maximum 1000). Restore appends a revision; it never deletes history.</p>
      <Button disabled={busy || !!retry || !target || !snapshot || !controlHeld} onClick={() => void request(true, JSON.stringify({ action: "restore", workspace_id: id, expected_revision: snapshot!.revision, target_revision: target, command_id: mintExperimentId() }))}>Restore selected revision</Button></>}
    {snapshot && onLoad && <Button disabled={busy || !!retry || !controlHeld} onClick={() => setMessage(onLoad(snapshot.document.plan) ? "Loaded as a new experiment. No settings applied or acquisition started." : "Cannot load while the current plan is running, pending, or locked. Make a new plan first.")}>Load as new experiment</Button>}
    {retry && <Button disabled={busy || !controlHeld} onClick={() => void request(true, retry)}>Retry same save</Button>}
    <details><summary>Pinned method and analysis recipe</summary>
      <p>Record immutable references supplied by your method/analysis provider. References are not resolved or scientifically validated on this runtime.</p>
      {!recipe && <Button disabled={busy || !!retry || !controlHeld} onClick={() => setRecipe({schema_version:1,id:mintExperimentId(),revision:"1",title:"Workspace recipe",question:"",method:{source:"local",id:"",revision:""},analysis_plan:{id:"",revision:""},lineage:{recipe:null,finding:null}})}>Add recipe references</Button>}
      {recipe && <>{(["title", "question"] as const).map(k => <label key={k}>{k}<input aria-label={`Recipe ${k}`} value={recipe[k]} disabled={busy || !!retry || !controlHeld} onChange={e => editRecipe({...recipe,[k]:e.target.value})}/></label>)}
      {(["method", "analysis_plan"] as const).map(k => <fieldset key={k}><legend>{k === "method" ? "Acquisition method" : "Analysis plan"}</legend>{(["id", "revision"] as const).map(f => <label key={f}>{f}<input aria-label={`${k} ${f}`} value={recipe[k][f]} disabled={busy || !!retry || !controlHeld} onChange={e => editRecipe({...recipe,[k]:{...recipe[k],[f]:e.target.value}})}/></label>)}</fieldset>)}
      <label>Method source <select aria-label="Method source" value={recipe.method.source} disabled={busy || !!retry || !controlHeld} onChange={e => editRecipe({...recipe,method:{...recipe.method,source:e.target.value as "local" | "registry"}})}><option value="local">Local</option><option value="registry">Registry</option></select></label>
      <RecipePairing recipe={snapshot?.document.recipe?.id === recipe.id ? recipe : {...recipe, revision:null}} contextRevision={null} compatibility={null}/></>}
    </details>
    {message && <Notice tone="info" title="Workspace persistence">{message}</Notice>}
    {snapshot && <><p>Workspace {snapshot.workspace_id} · immutable revision {snapshot.revision}</p><AnnotationGroups draft={snapshot.document.plan} /></>}
  </Card>;
}
