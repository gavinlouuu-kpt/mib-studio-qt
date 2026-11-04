// I01: a read-only projection of the existing planned annotation contract. No science,
// persistence, acquisition or alternative identity authority belongs here.
import { buildStartRequest, conditionSlots, type PlanDraft, type SampleRow } from "./planModel";
import type { ConditionSlot } from "@mib/ui";

export interface AnnotationLayer { id: string; label: string; field?: "condition" | "experimentalUnit" | "channel" | "medium"; freeKey?: string }
const FIXED: AnnotationLayer[] = [
  { id: "condition", label: "Condition", field: "condition" },
  { id: "experimentalUnit", label: "Experimental unit", field: "experimentalUnit" },
  { id: "channel", label: "Channel", field: "channel" },
  { id: "medium", label: "Medium", field: "medium" },
];

/** Free keys are namespaced, so a free key named 'condition' cannot shadow the fixed field. */
export function annotationLayers(rows: readonly SampleRow[]): AnnotationLayer[] {
  const keys = new Set(rows.flatMap(r => r.free.map(e => e.key.trim()).filter(Boolean)));
  return [...FIXED, ...[...keys].sort().map(key => ({ id: `free:${key}`, label: `Annotation · ${key}`, freeKey: key }))];
}

export function annotationValue(row: SampleRow, layer: AnnotationLayer): string | null {
  if (layer.field) return row[layer.field].trim() || null;
  const entries = row.free.filter(e => e.key.trim() === layer.freeKey);
  // Duplicate keys are invalid in the start contract: never silently pick one.
  return entries.length === 1 ? entries[0].value.trim() || null : null;
}

export interface AnnotationGroup {
  key: string;
  value: string | null;
  sampleRefs: string[];
  declaredUnits: string[];
  missingUnitSamples: number;
  conditionSlot?: ConditionSlot;
}

/** Counts are sheet rows and declared labels, not runs, cells or inferred independent replicates. */
export function annotationGroups(rows: readonly SampleRow[], layer: AnnotationLayer): AnnotationGroup[] {
  const groups = new Map<string, AnnotationGroup>();
  const slots = conditionSlots(rows);
  for (const row of rows) {
    const value = annotationValue(row, layer);
    const key = JSON.stringify(value); // null must differ from a literal 'Not declared' annotation.
    let group = groups.get(key);
    if (!group) {
      group = { key, value, sampleRefs: [], declaredUnits: [], missingUnitSamples: 0,
        conditionSlot: layer.id === "condition" && value !== null ? slots.get(value) : undefined };
      groups.set(key, group);
    }
    group.sampleRefs.push(row.ref.trim());
    const unit = row.experimentalUnit.trim();
    if (!unit) group.missingUnitSamples++;
    else if (!group.declaredUnits.includes(unit)) group.declaredUnits.push(unit);
  }
  return [...groups.values()];
}

/** Portable annotation projection, not a saved workspace or a recording/results export.
 * Reuses the production start serializer for validation, omission and UTF-8 bounds.
 * The temporary run index is NOT exported: no acquisition identity is minted here. */
export function annotationExport(plan: PlanDraft, layerId: string): { json: string | null; issues: string[] } {
  const layers = annotationLayers(plan.rows);
  const layer = layers.find(l => l.id === layerId) ?? layers[0];
  const issues: string[] = [];
  const seen = new Set<string>();
  const samples = plan.rows.map((row, index) => {
    if (seen.has(row.ref.trim())) issues.push(`${row.ref}: duplicate sample reference`);
    seen.add(row.ref.trim());
    const built = buildStartRequest(plan, index, 1);
    issues.push(...built.issues.map(i => `${row.ref}: ${i.label}: ${i.text}`));
    const request = built.json ? JSON.parse(built.json) as { annotation?: unknown } : null;
    return { sample_ref: row.ref.trim(), annotation: request?.annotation ?? null };
  });
  if (!samples.length) issues.push("The sample sheet is empty");
  if (issues.length) return { json: null, issues };
  return { issues: [], json: JSON.stringify({
    schema: "mib.planned-annotations", version: 1, experiment_id: plan.experimentId,
    evidence: "planned annotations only; not acquisition results or a saved workspace",
    selected_layer: layer.id, layers, samples,
    groups: annotationGroups(plan.rows, layer).map(g => ({
      value: g.value, sample_refs: g.sampleRefs, declared_unit_labels: g.declaredUnits,
      samples_without_unit: g.missingUnitSamples, condition_slot: g.conditionSlot ?? null,
    })),
  }, null, 2) };
}
