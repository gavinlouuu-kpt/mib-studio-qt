import { expect, it } from "vitest";
import fixture from "./fixtures/ique-recipe-v1.json";
import { copyRecipe, currentCompatibility, decodeRecipe, recipePairKey, type RecipeCompatibility } from "./recipeModel";
it("round-trips opaque revisions without numeric coercion", () => {
  expect(decodeRecipe(JSON.parse(JSON.stringify(fixture)))).toEqual(fixture);
  expect(decodeRecipe(fixture).revision).toBe("9007199254740993");
});
it("copies detached references into a fresh unsaved identity with lineage", () => {
  const source = decodeRecipe(fixture), before = JSON.stringify(source);
  const draft = copyRecipe(source, "new-draft");
  expect(draft.revision).toBeNull();
  expect(draft.lineage.recipe).toEqual({ id: source.id, revision: source.revision });
  expect(draft.lineage.finding).toEqual(source.lineage.finding);
  draft.method.revision = "edited"; draft.analysis_plan.id = "edited"; draft.lineage.finding!.id = "edited";
  expect(JSON.stringify(source)).toBe(before);
});
it("pins a new finding without aliasing it", () => {
  const finding = { id: "new-finding", revision: "rev" };
  const draft = copyRecipe(decodeRecipe(fixture), "new", finding);
  finding.id = "changed";
  expect(draft.lineage.finding!.id).toBe("new-finding");
});
it.each([null, {}, { ...fixture, schema_version: 2 }, { ...fixture, revision: 9 },
  { ...fixture, method: { ...fixture.method, source: "guess" } },
  { ...fixture, analysis_plan: { id: "plan", revision: " " } },
  { ...fixture, lineage: { recipe: { id: fixture.id, revision: fixture.revision }, finding: null } },
])("rejects invalid persisted association %#", value => expect(() => decodeRecipe(value)).toThrow());
it.each(["", " padded", "bad\nidentity", fixture.id])("rejects invalid/reused draft id %j", id => {
  expect(() => copyRecipe(decodeRecipe(fixture), id)).toThrow();
});
it("rejects stale compatibility after setup, draft, method or plan changes", () => {
  const recipe = decodeRecipe(fixture);
  const result: RecipeCompatibility = { pair_key: recipePairKey(recipe), context_revision: "setup-1", status: "compatible", reasons: [] };
  expect(currentCompatibility(recipe, "setup-1", result)).toBe(result);
  expect(currentCompatibility(recipe, "setup-2", result)).toBeNull();
  expect(currentCompatibility(recipe, null, result)).toBeNull();
  expect(currentCompatibility(copyRecipe(recipe, "new"), "setup-1", result)).toBeNull();
  expect(currentCompatibility({ ...recipe, method: { ...recipe.method, revision: "next" } }, "setup-1", result)).toBeNull();
  expect(currentCompatibility({ ...recipe, analysis_plan: { ...recipe.analysis_plan, revision: "next" } }, "setup-1", result)).toBeNull();
});

it("rejects malformed compatibility evidence instead of treating it as a verdict", () => {
  const recipe = decodeRecipe(fixture);
  const base = { pair_key: recipePairKey(recipe), context_revision: "setup", status: "compatible", reasons: [] };
  for (const bad of [{ ...base, status: "guessed" }, { ...base, reasons: null }, { ...base, reasons: [17] }])
    expect(currentCompatibility(recipe, "setup", bad as unknown as RecipeCompatibility)).toBeNull();
});
