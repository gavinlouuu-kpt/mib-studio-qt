import { useRef, useState } from "react";
import { Button, Card, Notice } from "@mib/ui";
import { invoke } from "../transport";
import { AnnotationGroups } from "./AnnotationGroups";
import type { PlanDraft } from "./planModel";
import { decodeSavedPlan } from "./savedPlanModel";

type Snapshot = { workspace_id: string; revision: string; document: { schema_version: 1; plan: PlanDraft } };
/** Production metadata client. No automatic reads/writes, acquisition actions or localStorage fallback. */
export function SavedPlan({ draft, controlHeld }: { draft: PlanDraft; controlHeld: boolean }) {
  const [id, setId] = useState(draft.experimentId);
  const [snapshot, setSnapshot] = useState<Snapshot | null>(null);
  const [message, setMessage] = useState("");
  const [busy, setBusy] = useState(false);
  const [retry, setRetry] = useState<string | null>(null);
  const pending = useRef(false);
  const request = async (write: boolean, payload: string) => {
    if (pending.current || (write && !controlHeld)) return;
    pending.current = true; setBusy(true); setMessage("");
    try {
      const reply = await invoke<unknown>(write ? "workspace_write" : "workspace_read", { request: payload });
      if (!reply || typeof reply !== "object" || !("status" in reply)) throw new Error("Invalid metadata reply");
      const r = reply as Record<string, unknown>;
      if (r.status !== "ok") {
        // Storage failures may have committed. Keep the exact idempotent command for reconciliation.
        setRetry(write && r.status === "storage" ? payload : null);
        setSnapshot(null);
        setMessage(`${String(r.status)}: ${typeof r.message === "string" ? r.message : "Workspace was not read or saved"}. Reopen before editing an existing revision.`);
        return;
      }
      const q = JSON.parse(payload);
      if (r.workspace_id !== q.workspace_id || typeof r.revision !== "string" || !/^[1-9][0-9]*$/.test(r.revision)) throw new Error("Invalid workspace identity in reply");
      const plan = decodeSavedPlan(r.document);
      setSnapshot({ workspace_id: r.workspace_id as string, revision: r.revision, document: { schema_version: 1, plan } });
      setRetry(null);
      setMessage(`${write ? "Saved" : "Opened"} revision ${r.revision}. This is a recorded plan, not proof of acquisition or scientific compatibility.`);
    } catch (e) {
      setSnapshot(null);
      if (write) setRetry(payload);
      setMessage(`${String(e)}${write ? ". Save outcome unknown; retry the same save to reconcile." : ""}`);
    } finally { pending.current = false; setBusy(false); }
  };
  const save = () => {
    const document = { schema_version: 1, plan: draft };
    try { decodeSavedPlan(document); } catch (e) { setMessage(String(e)); return; }
    void request(true, JSON.stringify({ action: "commit", workspace_id: id, expected_revision: snapshot?.revision ?? null,
      command_id: crypto.randomUUID(), document }));
  };
  return <Card title="Saved workspace plan" data-testid="saved-plan">
    <p>Save the sample sheet and declared operator to the app metadata database. Reopen to inspect annotations. Nothing is applied or started, and an opened plan does not replace the active experiment.</p>
    <label>Workspace ID <input aria-label="Saved workspace ID" value={id} disabled={busy || !!retry}
      onChange={e => { setId(e.target.value); setSnapshot(null); setMessage(""); }} /></label>
    <Button disabled={busy || !!retry || !id.trim()} onClick={() => void request(false, JSON.stringify({ action: "read", workspace_id: id }))}>Open saved plan</Button>
    <Button disabled={busy || !!retry || !controlHeld || !id.trim()} onClick={save}>{snapshot ? "Save new revision" : "Save new workspace"}</Button>
    {retry && <Button disabled={busy || !controlHeld} onClick={() => void request(true, retry)}>Retry same save</Button>}
    {message && <Notice tone="info" title="Workspace persistence">{message}</Notice>}
    {snapshot && <><p>Workspace {snapshot.workspace_id} · immutable revision {snapshot.revision}</p><AnnotationGroups draft={snapshot.document.plan} /></>}
  </Card>;
}
