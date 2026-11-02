// Experiment · Plan: the recipe library (#807 I02, fold-in slice I-1). A view over the native recipe
// provider (bridge ABI 50): it lists, reads, creates and derives immutable recipes, and shows the
// backend's compatibility diff and apply preview. It computes nothing and applies nothing: the
// method selection a preview names must be accepted explicitly by the method authority (the
// methods panel or the local profile store), and core and calibration are never changed here.
import { useState } from "react";
import { Button, Card, Notice } from "@mib/ui";
import { bridge } from "../bridge";
import { mintExperimentId } from "./planModel";
import { RecipePairing } from "./RecipePairing";
import { decodeApplyPreview, decodeRecipeDiff, decodeRecipeList, decodeRecipeReply, pairingView,
  type ProviderRecipe, type RecipeApplyPreview, type RecipeDiff, type RecipeSummary } from "./recipeModel";

type Pending = { kind: "create" | "derive"; request: Record<string, unknown> };
const blank = { title: "", note: "", source: "local" as "local" | "registry", methodId: "", methodRevision: "", planId: "", planRevision: "" };

export function RecipeLibrary({ controlHeld }: { controlHeld: boolean }) {
  const [profileBase, setProfileBase] = useState("");
  const [recipes, setRecipes] = useState<RecipeSummary[]>([]);
  const [after, setAfter] = useState<string | null>(null);
  const [selected, setSelected] = useState<ProviderRecipe | null>(null);
  const [diff, setDiff] = useState<RecipeDiff | null>(null);
  const [preview, setPreview] = useState<RecipeApplyPreview | null>(null);
  const [form, setForm] = useState(blank);
  const [busy, setBusy] = useState(false);
  const [retry, setRetry] = useState<Pending | null>(null);
  const [message, setMessage] = useState("");
  const base = profileBase.trim() ? { profile_base: profileBase.trim() } : {};

  const run = async (work: () => Promise<void>) => {
    if (busy) return;
    setBusy(true); setMessage("");
    try { await work(); } catch (e) { setMessage(String(e instanceof Error ? e.message : e)); } finally { setBusy(false); }
  };
  const open = (id: string, revision: string) => run(async () => {
    setSelected(decodeRecipeReply(await bridge.recipeGet({ recipe_id: id, revision })));
    setDiff(null); setPreview(null);
  });
  const list = (cursor: string | null) => run(async () => {
    const page = decodeRecipeList(await bridge.recipeList(cursor ? { after: cursor, limit: 50 } : { limit: 50 }));
    setRecipes(prev => cursor ? [...prev, ...page.recipes] : page.recipes); setAfter(page.next_after);
  });
  const write = (pending: Pending) => run(async () => {
    setRetry(pending);
    const reply = pending.kind === "create" ? await bridge.recipeCreate(pending.request) : await bridge.recipeDerive(pending.request);
    let recipe: ProviderRecipe;
    try { recipe = decodeRecipeReply(reply); } catch (e) {
      // A refusal is final; only a lost or malformed reply keeps the exact command for a retry.
      if (reply && typeof reply === "object" && "status" in reply && (reply as { status: unknown }).status !== "storage") setRetry(null);
      throw e;
    }
    setRetry(null); setSelected(recipe); setDiff(null); setPreview(null);
    setMessage(`${pending.kind === "create" ? "Created" : "Derived"} recipe ${recipe.id} · revision ${recipe.revision}. Nothing was applied.`);
  });
  const compare = (against: "instrument" | { recipe_id: string; revision: string }) => selected && run(async () => {
    setDiff(decodeRecipeDiff(await bridge.recipeDiff({ recipe_id: selected.id, revision: selected.revision, against, ...base })));
  });
  const previewApply = () => selected && run(async () => {
    setPreview(decodeApplyPreview(await bridge.recipeApplyPreview({ recipe_id: selected.id, revision: selected.revision, ...base })));
  });
  const blocked = busy || !!retry;

  return <Card title="Recipe library" data-testid="recipe-library">
    <p>Recipes pair an acquisition method revision with an analysis plan revision. They are stored by the backend and never edited: “Base a new draft” creates a new recipe linked to its parent. Compatibility and apply previews are computed by the backend; nothing here applies a method, calibration or hardware setting.</p>
    <label>Local profile directory <input aria-label="Recipe profile directory" value={profileBase} disabled={busy} onChange={e => { setProfileBase(e.target.value); setDiff(null); setPreview(null); }} /></label>
    <Button disabled={blocked} onClick={() => void list(null)}>List recipes</Button>
    {after && <Button disabled={blocked} onClick={() => void list(after)}>More recipes</Button>}
    <ul>{recipes.map(r => <li key={`${r.id}:${r.revision}`}><Button disabled={blocked} onClick={() => void open(r.id, r.revision)}>{r.title || r.id} · {r.method.id} @ {r.method.revision} + {r.analysis_plan.id} @ {r.analysis_plan.revision}</Button></li>)}</ul>
    <details><summary>New recipe</summary>
      {(["title", "note", "methodId", "methodRevision", "planId", "planRevision"] as const).map(k =>
        <label key={k}>{k}<input aria-label={`New recipe ${k}`} value={form[k]} disabled={blocked || !controlHeld} onChange={e => setForm({ ...form, [k]: e.target.value })} /></label>)}
      <label>Method source <select aria-label="New recipe method source" value={form.source} disabled={blocked || !controlHeld} onChange={e => setForm({ ...form, source: e.target.value as "local" | "registry" })}><option value="local">Local</option><option value="registry">Registry</option></select></label>
      <Button disabled={blocked || !controlHeld} onClick={() => void write({ kind: "create", request: { command_id: mintExperimentId(), recipe_id: mintExperimentId(), title: form.title, note: form.note,
        method: { source: form.source, id: form.methodId, revision: form.methodRevision }, analysis_plan: { id: form.planId, revision: form.planRevision }, ...base } })}>Create recipe</Button>
    </details>
    {retry && <Button disabled={busy || !controlHeld} onClick={() => void write(retry)}>Retry the same {retry.kind}</Button>}
    {selected && <>
      <RecipePairing recipe={pairingView(selected)} contextRevision={null} compatibility={null} diff={diff}
        onCopy={controlHeld && !blocked ? () => void write({ kind: "derive", request: { command_id: mintExperimentId(), recipe_id: mintExperimentId(),
          parent: { id: selected.id, revision: selected.revision }, ...base } }) : undefined} />
      <Button disabled={blocked} onClick={() => void compare("instrument")}>Compare with the instrument</Button>
      {selected.lineage.recipe && <Button disabled={blocked} onClick={() => void compare({ recipe_id: selected.lineage.recipe!.id, revision: selected.lineage.recipe!.revision })}>Compare with the parent recipe</Button>}
      <Button disabled={blocked} onClick={() => void previewApply()}>Preview apply</Button>
    </>}
    {preview && <Notice tone={preview.blocked.length ? "warn" : "info"} title={`Apply preview · ${preview.overall} · not applied`}>
      <p data-testid="apply-preview-selection">{preview.method_selection
        ? `Method selection to send to ${preview.method_selection.authority}: ${preview.method_selection.command} ${JSON.stringify(preview.method_selection.args)}. It takes effect only when that authority accepts it.`
        : preview.blocked.length ? "No method selection can be prepared." : "The pinned method is already the applied one."}</p>
      <ul>{preview.changes.map(c => <li key={c.dimension}>{c.dimension}: {JSON.stringify(c.from)} → {JSON.stringify(c.to)}</li>)}</ul>
      <ul>{preview.not_changed_by_apply.map(c => <li key={c.dimension}>{c.dimension} ({c.status}): {c.reason}. {c.action}.</li>)}</ul>
      <ul>{preview.blocked.map((b, i) => <li key={i}>{b}</li>)}</ul>
      <p>Analysis plan {preview.analysis_plan.id} · {preview.analysis_plan.revision}: {preview.analysis_plan.effect}.</p>
    </Notice>}
    {message && <Notice tone="info" title="Recipe provider">{message}</Notice>}
  </Card>;
}
