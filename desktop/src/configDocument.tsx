import { invoke } from "@tauri-apps/api/core";
import { open } from "@tauri-apps/plugin-dialog";
import { useRef, useState } from "react";

export interface ConfigDocument { ok: boolean; path: string; revision: string; document_json: string; error: string }
export interface ConfigTransaction { saved: boolean; applied: boolean; verified: boolean; conflict: boolean; revision: string; error: string }
export const configDocument = {
  read: (path: string) => invoke<ConfigDocument>("fetch_config_document", {path}),
  apply: (path: string, baseline: string, patch: string) => invoke<ConfigTransaction>("apply_config_document", {path, baseline, patch}),
};

export function changedProcessingFields(baseline: Record<string, unknown>, edited: Record<string, unknown>): Record<string, unknown> {
  const changes: Array<[string, unknown]> = [];
  for (const key of Object.keys(baseline)) if (!(key in edited)) throw new Error(`Removing setting ${key} is not supported; use its explicit value.`);
  for (const [key, value] of Object.entries(edited)) {
    const previous = baseline[key];
    if (JSON.stringify(previous) === JSON.stringify(value)) continue;
    if (value && previous && typeof value === "object" && typeof previous === "object" && !Array.isArray(value) && !Array.isArray(previous)) {
      const nested = changedProcessingFields(previous as Record<string,unknown>, value as Record<string,unknown>);
      if (Object.keys(nested).length) changes.push([key,nested]);
    } else changes.push([key,value]);
  }
  return Object.fromEntries(changes);
}

// State is retained by App across navigation. Only image_processing is editable:
// the shared transaction preserves unrelated and unknown document fields.
export function useConfigDocument({ ready, active, append, refresh }: { ready: boolean; active: boolean; append: (s:string)=>void; refresh:()=>Promise<void> }) {
  const [doc, setDoc] = useState<ConfigDocument | null>(null);
  const [draft, setDraft] = useState("");
  const [dirty, setDirty] = useState(false);
  const [busy, setBusy] = useState(false);
  const [result, setResult] = useState<ConfigTransaction | null>(null);
  const [error, setError] = useState("");
  const pending = useRef(false);
  const blocked = !ready || active || busy;
  const run = async (action: "open" | "reload" | "apply", selectedPath?:string) => {
    if (!ready || active || pending.current) return;
    if (action !== "apply" && dirty && !window.confirm("Discard unsaved config edits and reload?")) return;
    pending.current = true; setBusy(true); setError("");
    try {
      if (action === "apply" && doc) {
        // Parse locally for a useful syntax error; the backend owns validation.
        const parsed: unknown = JSON.parse(draft);
        if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) throw new Error("Processing settings must be a JSON object");
        const original = JSON.parse(doc.document_json);
        const patch = changedProcessingFields(original.image_processing ?? {}, parsed as Record<string,unknown>);
        if (!Object.keys(patch).length) { setDirty(false); return; }
        const r = await configDocument.apply(doc.path, doc.revision, JSON.stringify({image_processing:patch}));
        setResult(r);
        append(`Config: saved=${r.saved}, applied=${r.applied}, verified=${r.verified}${r.error ? ` — ${r.error}` : ""}`);
        // Conflict/failure keeps the draft and original baseline. Reload/reconcile
        // is explicit; never silently retry against a new revision.
        if (r.saved && r.applied && r.verified) {
          setDoc({...doc, revision:r.revision, document_json:JSON.stringify({...original,image_processing:parsed})}); setDirty(false); await refresh();
        }
        return;
      }
      const path = action === "reload" ? doc?.path : selectedPath ?? await open({multiple:false, filters:[{name:"Application configuration",extensions:["json"]}]});
      if (typeof path !== "string") return;
      const loaded = await configDocument.read(path);
      if (!loaded.ok) throw new Error(loaded.error);
      const parsed = JSON.parse(loaded.document_json);
      setDoc(loaded); setDraft(JSON.stringify(parsed.image_processing ?? {},null,2)); setDirty(false); setResult(null);
    } catch (e) { setError(String(e)); append(`Config: ${String(e)}`); }
    finally { pending.current=false; setBusy(false); }
  };
  return {doc,draft,dirty,busy,blocked,result,error,run,edit:(text:string)=>{setDraft(text);setDirty(true);}};
}

export function ConfigDocumentEditor({model}:{model:ReturnType<typeof useConfigDocument>}) {
  return <section aria-label="Persistent processing configuration">
    <h5>Saved processing configuration</h5>
    <div className="toolbar">
      <button disabled={model.blocked} onClick={()=>void model.run("open")}>Open Config…</button>
      <button disabled={model.blocked || !model.doc} onClick={()=>void model.run("reload")}>Reload / Revert</button>
      <button disabled={model.blocked || !model.doc || !model.dirty} onClick={()=>void model.run("apply")}>Save and Apply</button>
    </div>
    <p className="mono">{model.doc?.path || "Select an existing application JSON file. Only image_processing will be changed."}</p>
    {model.doc && <textarea className="script-editor" aria-label="Saved image processing JSON" value={model.draft} disabled={model.blocked} onChange={e=>model.edit(e.target.value)} />}
    <p role="status">{model.busy ? "Config transaction in progress…" : model.error || (model.result ? `${model.result.conflict ? "Conflict — reload and reconcile. " : ""}Saved: ${model.result.saved}; applied: ${model.result.applied}; verified: ${model.result.verified}. ${model.result.error}` : model.dirty ? "Unsaved changes" : "")}</p>
    <p>This saves processing settings to the selected file. It does not change startup file selection or save ROI, calibration, camera, or realtime settings.</p>
  </section>;
}
