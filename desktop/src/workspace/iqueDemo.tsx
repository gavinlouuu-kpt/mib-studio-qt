// Development-only entry point; not added to either production app or Vite's production inputs.
// Uses the real presentation components with visibly labelled deterministic fixtures, no IPC.
import { useState } from "react";
import { createRoot } from "react-dom/client";
import { Button, Notice } from "@mib/ui";
import { AnnotationGroups } from "./AnnotationGroups";
import { RecipePairing } from "./RecipePairing";
import { copyRecipe, decodeRecipe, recipePairKey, type RecipeDraft } from "./recipeModel";
import recipeFixture from "./fixtures/ique-recipe-v1.json";
import { CalibrationTrends } from "./CalibrationTrends";
import { calRecord } from "./calibrationFixtures";
import annotations from "./fixtures/ique-annotations-v1.json";
import type { PlanDraft } from "./planModel";
import "./workspace.css";

const recipe = decodeRecipe(recipeFixture);
const plan: PlanDraft = { ...annotations.plan, format: "queue" };
const records = [
  calRecord("fixture-qc-1", "superseded", { created_at_utc: "2026-10-20T08:00:00Z" }),
  calRecord("fixture-qc-2", "applied", { created_at_utc: "2026-10-21T08:00:00Z" }),
  calRecord("fixture-qc-3", "insufficient", { created_at_utc: "2026-10-22T08:00:00Z" }),
];

function Demo() {
  const [selected, setSelected] = useState<string>();
  const [recipeDraft, setRecipeDraft] = useState<RecipeDraft | null>(null);
  const [changedSetup, setChangedSetup] = useState(false);
  const [failed, setFailed] = useState(false);
  return <main style={{ padding: 24, maxWidth: 1200, margin: "auto" }}>
    <h1>iQue module · fixture demonstration</h1>
    <Notice tone="warn" title="Synthetic fixtures only · no instrument or analysis worker connected">
      Annotation fixture v1 and calibration schema v1. No backend calls, acquisition, hardware application or scientific validation.
      Production Plan/Prepare views use their existing controllers; this isolated development entry demonstrates presentation only.
    </Notice>
    <AnnotationGroups draft={plan} />
    <RecipePairing recipe={recipeDraft ?? recipe} contextRevision={changedSetup ? "fixture-setup-2" : "fixture-setup-1"}
      compatibility={{ pair_key: recipePairKey(recipe), context_revision: "fixture-setup-1", status: "incompatible",
        reasons: ["Synthetic incompatibility: required parameter is absent"] }}
      onCopy={() => setRecipeDraft(copyRecipe(recipe, "fixture-new-draft"))} />
    <Button onClick={() => setChangedSetup(v => !v)}>Change fixture setup revision</Button>
    <Button onClick={() => { setRecipeDraft(null); setChangedSetup(false); }}>Reset recipe fixture</Button>
    <Button onClick={() => { setFailed(v => !v); setSelected(undefined); }}>
      {failed ? "Restore fixture read" : "Simulate failed calibration read"}
    </Button>
    {failed ? <Notice tone="blocked" title="Fixture read failed">Previous calibration values have been cleared.</Notice>
      : <CalibrationTrends records={records} selectedId={selected} onSelect={setSelected} />}
    <p role="status">{selected ? `Fixture record selected: ${selected} · production uses calibration_record_get, never Apply` : "No record selected"}</p>
  </main>;
}

createRoot(document.getElementById("root")!).render(<Demo />);
