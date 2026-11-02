import type { PlanDraft } from "./planModel";
const obj = (v: unknown): v is Record<string, unknown> => !!v && typeof v === "object" && !Array.isArray(v);
/** Structural metadata validation, not run readiness or scientific validation. */
export function decodeSavedPlan(value: unknown): PlanDraft {
  if (!obj(value) || value.schema_version !== 1 || !obj(value.plan)) throw new Error("Unsupported saved plan");
  const p = value.plan;
  if (typeof p.experimentId !== "string" || !/^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$/.test(p.experimentId)
    || !["single", "queue"].includes(p.format as string) || !obj(p.operator)
    || typeof p.operator.id !== "string" || typeof p.operator.name !== "string"
    || !Array.isArray(p.rows) || !p.rows.length || p.rows.length > 1000) throw new Error("Invalid saved plan identity or sheet");
  const refs = new Set<string>();
  for (const row of p.rows) {
    if (!obj(row) || !["ref", "sample", "condition", "experimentalUnit", "concentrationValue", "concentrationUnit", "channel", "medium", "temperatureC", "notes"].every(k => typeof row[k] === "string")
      || !row.ref || refs.has(row.ref as string) || !Array.isArray(row.free)
      || !row.free.every(e => obj(e) && typeof e.key === "string" && typeof e.value === "string")) throw new Error("Invalid saved sample row");
    refs.add(row.ref as string);
  }
  if (p.format === "single" && p.rows.length !== 1) throw new Error("Single-sample plan has multiple rows");
  return structuredClone(p) as unknown as PlanDraft;
}

/** Preserve all workspace-owned payloads, validate fields this client renders. */
export function decodeWorkspaceDocument(value: unknown): Record<string, unknown> & {schema_version:1;plan:PlanDraft} {
  const plan=decodeSavedPlan(value);
  const doc=value as Record<string,unknown>;
  if (doc.title!==undefined && (typeof doc.title!=="string" || doc.title.length>512)) throw new Error("Invalid workspace title");
  if (doc.archived!==undefined && typeof doc.archived!=="boolean") throw new Error("Invalid workspace archive state");
  if (doc.sources!==undefined && (!obj(doc.sources) || !Object.entries(doc.sources).every(([id,v])=>id.length>0 && id.length<=256 && obj(v) && typeof v.path==="string" && typeof v.sha256==="string" && /^[0-9a-f]{64}$/.test(v.sha256) && typeof v.size_bytes==="string" && /^(0|[1-9][0-9]*)$/.test(v.size_bytes)))) throw new Error("Invalid workspace source links");
  return {...structuredClone(doc),schema_version:1,plan};
}
