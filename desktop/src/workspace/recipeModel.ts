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
/** Consumer proposal, not a deployed bridge API or a persistence implementation. */
export interface RecipeProvider {
  read(id: string, revision: string): Promise<Recipe>;
  compatibility(recipe: Recipe | RecipeDraft, contextRevision: string): Promise<RecipeCompatibility>;
  create(draft: RecipeDraft, workspaceRevision: string, commandId: string): Promise<
    { status: "saved"; recipe: Recipe; workspace_revision: string }
    | { status: "conflict"; workspace_revision: string }
    | { status: "invalid" | "unavailable" | "unknown"; reason: string }>;
}
