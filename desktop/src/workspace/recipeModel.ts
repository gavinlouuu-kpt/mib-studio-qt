/** Association only: scientific validation and acquisition apply remain provider-owned. */
export interface RevisionRef { id: string; revision: string }
export interface MethodRef extends RevisionRef { source: "local" | "registry" }
export interface Recipe {
  schema_version: 1; id: string; revision: string; title: string; question: string;
  method: MethodRef; analysis_plan: RevisionRef;
  lineage: { recipe: RevisionRef | null; finding: RevisionRef | null };
}
export interface RecipeDraft extends Omit<Recipe, "revision"> { revision: null }
const object = (v: unknown): v is Record<string, unknown> => !!v && typeof v === "object" && !Array.isArray(v);
const token = (v: unknown): v is string => typeof v === "string" && v.length > 0 && v === v.trim() && !/[\u0000-\u001f\u007f]/.test(v);
const ref = (v: unknown): v is RevisionRef => object(v) && token(v.id) && token(v.revision);
const copyRef = (v: RevisionRef | null): RevisionRef | null => v === null ? null : { id: v.id, revision: v.revision };
/** Decode persisted input without coercing opaque revisions or retaining caller-owned objects. */
export function decodeRecipe(value: unknown): Recipe {
  if (!object(value) || value.schema_version !== 1 || !token(value.id) || !token(value.revision)
    || typeof value.title !== "string" || !value.title.trim() || typeof value.question !== "string"
    || !ref(value.method) || !("source" in value.method) || !["local", "registry"].includes(value.method.source as string)
    || !ref(value.analysis_plan) || !object(value.lineage)
    || !(value.lineage.recipe === null || ref(value.lineage.recipe))
    || !(value.lineage.finding === null || ref(value.lineage.finding))) {
    throw new Error("Invalid recipe v1: immutable method, analysis plan and lineage references are required");
  }
  if (value.lineage.recipe?.id === value.id && value.lineage.recipe.revision === value.revision)
    throw new Error("A recipe cannot be its own source revision");
  const method = value.method as MethodRef;
  return { schema_version: 1, id: value.id, revision: value.revision, title: value.title, question: value.question,
    method: { source: method.source, id: method.id, revision: method.revision },
    analysis_plan: { id: value.analysis_plan.id, revision: value.analysis_plan.revision },
    lineage: { recipe: copyRef(value.lineage.recipe), finding: copyRef(value.lineage.finding) } };
}
/** Identity supplied by caller; copy neither saves nor applies a method. */
export function copyRecipe(source: Recipe, newId: string, finding?: RevisionRef): RecipeDraft {
  const original = decodeRecipe(source);
  if (!token(newId) || newId === original.id) throw new Error("Copy requires a fresh draft identity");
  if (finding !== undefined && !ref(finding)) throw new Error("Finding requires an immutable revision");
  return { ...original, id: newId, revision: null, lineage: {
    recipe: { id: original.id, revision: original.revision }, finding: copyRef(finding ?? original.lineage.finding) } };
}
export function recipePairKey(recipe: Recipe | RecipeDraft): string {
  return JSON.stringify([recipe.id, recipe.revision, recipe.method.source, recipe.method.id,
    recipe.method.revision, recipe.analysis_plan.id, recipe.analysis_plan.revision]);
}
export interface RecipeCompatibility {
  pair_key: string; context_revision: string;
  status: "compatible" | "incompatible" | "unknown"; reasons: string[];
}
export function currentCompatibility(recipe: Recipe | RecipeDraft, contextRevision: string | null,
  result: RecipeCompatibility | null): RecipeCompatibility | null {
  return contextRevision !== null && token(contextRevision) && result?.pair_key === recipePairKey(recipe)
    && result.context_revision === contextRevision
    && ["compatible", "incompatible", "unknown"].includes(result.status)
    && Array.isArray(result.reasons) && result.reasons.every(reason => typeof reason === "string") ? result : null;
}
/** Bridge ABI 50 (#807 I02): the native recipe provider's records and replies. These decoders only
 *  check shape; every comparison, diff and apply decision is the backend's (RecipeProvider.cpp). */
export type DiffStatus = "equal" | "different" | "unknown";
export interface DiffItem { dimension: "method" | "profile" | "core" | "calibration"; status: DiffStatus; reason: string; left: unknown; right: unknown }
export interface ProviderRecipe {
  schema_version: 1; type: "I-recipe/1"; id: string; revision: string; title: string; note: string; created_at: string;
  method: MethodRef & { content_sha256: string }; analysis_plan: RevisionRef;
  lineage: { recipe: RevisionRef | null; finding: RevisionRef | null };
  basis: Record<string, unknown>;
}
export interface RecipeSummary { id: string; revision: string; title: string; created_at: string; method: MethodRef; analysis_plan: RevisionRef; lineage: { recipe: RevisionRef | null; finding: RevisionRef | null } }
export interface RecipeDiff { left: { kind: "recipe"; id: string; revision: string }; right: Record<string, unknown>; items: DiffItem[]; overall: DiffStatus }
export interface RecipeApplyPreview {
  recipe: RevisionRef; readiness_generation: string; items: DiffItem[]; overall: DiffStatus;
  changes: { dimension: string; from: unknown; to: unknown; via: string | null }[];
  method_selection: null | { authority: string; command: string; args: Record<string, unknown> };
  not_changed_by_apply: { dimension: string; status: DiffStatus; reason: string; action: string }[];
  blocked: string[]; analysis_plan: RevisionRef & { effect: string };
  applied: false; hardware_actuated: false; requires_explicit_acceptance: true;
}
/** A provider refusal ({status != ok, message}) becomes an Error carrying the backend's words. */
function ok(reply: unknown): Record<string, unknown> {
  if (!object(reply) || typeof reply.status !== "string") throw new Error("Invalid recipe provider reply");
  if (reply.status !== "ok") throw new Error(`${reply.status}: ${typeof reply.message === "string" ? reply.message : "refused"}`);
  return reply;
}
const diffItems = (v: unknown): DiffItem[] => {
  if (!Array.isArray(v) || v.length !== 4 || !v.every(i => object(i) && typeof i.dimension === "string"
    && ["equal", "different", "unknown"].includes(i.status as string) && typeof i.reason === "string" && i.reason.length > 0))
    throw new Error("Invalid recipe diff items");
  return v as DiffItem[];
};
const providerRecipe = (v: unknown): ProviderRecipe => {
  if (!object(v) || v.type !== "I-recipe/1" || !token(v.id) || !token(v.revision) || typeof v.title !== "string"
    || !ref(v.method) || !ref(v.analysis_plan) || !object(v.lineage)) throw new Error("Invalid provider recipe");
  return v as unknown as ProviderRecipe;
};
export const decodeRecipeList = (reply: unknown): { recipes: RecipeSummary[]; next_after: string | null } => {
  const r = ok(reply);
  if (!Array.isArray(r.recipes) || !r.recipes.every(x => object(x) && token(x.id) && token(x.revision) && typeof x.title === "string"))
    throw new Error("Invalid recipe listing");
  return { recipes: r.recipes as RecipeSummary[], next_after: typeof r.next_after === "string" ? r.next_after : null };
};
export const decodeRecipeReply = (reply: unknown): ProviderRecipe => providerRecipe(ok(reply).recipe);
export const decodeRecipeDiff = (reply: unknown): RecipeDiff => {
  const r = ok(reply);
  if (!object(r.left) || !object(r.right) || !["equal", "different", "unknown"].includes(r.overall as string)) throw new Error("Invalid recipe diff");
  return { left: r.left as RecipeDiff["left"], right: r.right, items: diffItems(r.items), overall: r.overall as DiffStatus };
};
export const decodeApplyPreview = (reply: unknown): RecipeApplyPreview => {
  const r = ok(reply);
  if (r.applied !== false || r.hardware_actuated !== false || r.requires_explicit_acceptance !== true
    || !Array.isArray(r.changes) || !Array.isArray(r.not_changed_by_apply) || !Array.isArray(r.blocked))
    throw new Error("Invalid apply preview: a preview must not apply or actuate");
  diffItems(r.items);
  return r as unknown as RecipeApplyPreview;
};
/** The pinned pair as RecipePairing shows it (identity only; nothing is computed). */
export const pairingView = (r: ProviderRecipe): Recipe => ({ schema_version: 1, id: r.id, revision: r.revision, title: r.title,
  question: r.note, method: { source: r.method.source, id: r.method.id, revision: r.method.revision },
  analysis_plan: { id: r.analysis_plan.id, revision: r.analysis_plan.revision }, lineage: r.lineage });
