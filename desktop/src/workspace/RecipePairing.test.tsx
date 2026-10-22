// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { RecipePairing } from "./RecipePairing";
import { copyRecipe, decodeRecipe, recipePairKey, type RecipeCompatibility } from "./recipeModel";
import fixture from "./fixtures/ique-recipe-v1.json";
let host: HTMLDivElement, root: Root;
beforeEach(() => { Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true }); host = document.createElement("div"); document.body.append(host); root = createRoot(host); });
afterEach(() => { act(() => root.unmount()); host.remove(); });
const recipe = decodeRecipe(fixture);
const compatibility: RecipeCompatibility = { pair_key: recipePairKey(recipe), context_revision: "setup", status: "incompatible", reasons: ["Parameter absent in this method"] };
it("shows pinned references and provider reason; never acts on render", () => {
  const copy = vi.fn();
  act(() => root.render(<RecipePairing recipe={recipe} contextRevision="setup" compatibility={compatibility} onCopy={copy} />));
  expect(host.textContent).toContain("Parameter absent in this method");
  expect(host.textContent).toContain("9007199254740993");
  expect(copy).not.toHaveBeenCalled();
  expect(host.querySelectorAll("button")).toHaveLength(1);
  act(() => host.querySelector("button")!.click());
  expect(copy).toHaveBeenCalledOnce();
});
it("clears compatibility after a context change and does not expose apply", () => {
  act(() => root.render(<RecipePairing recipe={recipe} contextRevision="new-setup" compatibility={compatibility} />));
  expect(host.textContent).toContain("Compatibility not checked");
  expect(host.textContent).not.toContain("Parameter absent");
  expect(host.querySelectorAll("button")).toHaveLength(0);
});
it("marks a copied draft unsaved and suppresses copy-from-uncommitted", () => {
  act(() => root.render(<RecipePairing recipe={copyRecipe(recipe, "fresh")} contextRevision="setup" compatibility={compatibility} onCopy={vi.fn()} />));
  expect(host.textContent).toContain("not saved or applied");
  expect(host.textContent).toContain("Compatibility not checked");
  expect(host.querySelectorAll("button")).toHaveLength(0);
});
