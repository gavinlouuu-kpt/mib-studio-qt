// @vitest-environment jsdom
// #807 I02: the production recipe view binds to the native recipe provider (bridge ABI 50) and
// shows its replies as returned. It computes no diff and sends no apply.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import librarySource from "./RecipeLibrary.tsx?raw";
import pairingSource from "./RecipePairing.tsx?raw";
import planSource from "./PlanView.tsx?raw";
import modelSource from "./recipeModel.ts?raw";
import { afterEach, beforeEach, expect, it, vi } from "vitest";
vi.mock("../bridge", () => ({ bridge: {
  recipeList: vi.fn(), recipeGet: vi.fn(), recipeCreate: vi.fn(), recipeDerive: vi.fn(),
  recipeDiff: vi.fn(), recipeApplyPreview: vi.fn(),
} }));
import { bridge } from "../bridge";
import { RecipeLibrary } from "./RecipeLibrary";

const b = vi.mocked(bridge);
let host: HTMLDivElement, root: Root;
beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
  vi.clearAllMocks();
});
afterEach(() => { act(() => root.unmount()); host.remove(); });

const recipe = (id: string, parent: { id: string; revision: string } | null = null) => ({
  schema_version: 1, type: "I-recipe/1", id, revision: "1", title: "Bead QC", note: "baseline", created_at: "2026-11-02T10:00:00.000Z",
  method: { source: "local", id: "beads", revision: "rev-a", content_sha256: "rev-a" }, analysis_plan: { id: "plan-1", revision: "1" },
  lineage: { recipe: parent, finding: null }, basis: { calibration_known: true },
});
const items = [
  { dimension: "method", status: "equal", reason: "The instrument runs local beads @ rev-a", left: null, right: null },
  { dimension: "profile", status: "equal", reason: "Same canonical config.json (cfg-a)", left: null, right: null },
  { dimension: "core", status: "unknown", reason: "Backend says the core is unknown because X", left: null, right: null },
  { dimension: "calibration", status: "different", reason: "Backend calibration words", left: null, right: null },
];
const flush = async () => { for (let i = 0; i < 5; ++i) await act(async () => { await Promise.resolve(); }); };
const button = (text: string) => [...host.querySelectorAll("button")].find(x => x.textContent?.includes(text)) as HTMLButtonElement | undefined;
const click = async (text: string) => { const el = button(text); expect(el, text).toBeTruthy(); await act(async () => el!.click()); await flush(); };

async function openOriginal(controlHeld = true) {
  b.recipeList.mockResolvedValue({ status: "ok", recipes: [recipe("r-original")], next_after: null });
  b.recipeGet.mockResolvedValue({ status: "ok", recipe: recipe("r-original") });
  act(() => root.render(<RecipeLibrary controlHeld={controlHeld} />));
  await click("List recipes");
  await click("beads @ rev-a");
  expect(b.recipeGet).toHaveBeenCalledWith({ recipe_id: "r-original", revision: "1" });
}

it("lists, opens and shows the backend's diff verbatim", async () => {
  await openOriginal();
  expect(b.recipeList).toHaveBeenCalledWith({ limit: 50 });
  b.recipeDiff.mockResolvedValue({ status: "ok", left: { kind: "recipe", id: "r-original", revision: "1" }, right: { kind: "instrument", readiness_generation: "7" }, items, overall: "different", applied: false });
  await click("Compare with the instrument");
  expect(b.recipeDiff).toHaveBeenCalledWith({ recipe_id: "r-original", revision: "1", against: "instrument" });
  const diff = host.querySelector("[data-testid=recipe-diff]")!.textContent!;
  expect(diff).toContain("Backend says the core is unknown because X");
  expect(diff).toContain("calibration · different · Backend calibration words");
  expect(host.textContent).toContain("Compatibility with the instrument: different");
});

it("derives a new recipe from the selected one with fresh identities and the parent link", async () => {
  await openOriginal();
  b.recipeDerive.mockResolvedValue({ status: "ok", replayed: false, recipe: recipe("r-next", { id: "r-original", revision: "1" }) });
  await click("Base a new draft on this recipe");
  const request = b.recipeDerive.mock.calls[0][0] as Record<string, unknown>;
  expect(request.parent).toEqual({ id: "r-original", revision: "1" });
  expect(request.recipe_id).not.toBe("r-original");
  expect(request.command_id).toMatch(/^[0-9a-f-]{36}$/);
  expect(host.textContent).toContain("Derived recipe r-next");
  expect(host.textContent).toContain("r-original · 1");
  expect(button("Compare with the parent recipe")).toBeTruthy();
});

it("a viewer cannot derive or create", async () => {
  await openOriginal(false);
  expect(button("Base a new draft on this recipe")).toBeUndefined();
  expect(button("Create recipe")!.disabled).toBe(true);
});

it("apply preview shows the method selection and never sends it", async () => {
  await openOriginal();
  b.recipeApplyPreview.mockResolvedValue({ status: "ok", recipe: { id: "r-original", revision: "1" }, readiness_generation: "7", items, overall: "different",
    changes: [{ dimension: "method", from: { id: "beads", revision: "rev-z" }, to: { id: "beads", revision: "rev-a" }, via: "profile_command" }],
    method_selection: { authority: "local_profile_store", command: "profile_command", args: { base: "/p", request: "{}" } },
    not_changed_by_apply: [{ dimension: "calibration", status: "different", reason: "Backend calibration words", action: "Apply never changes calibration" }],
    blocked: [], analysis_plan: { id: "plan-1", revision: "1", effect: "nothing on the instrument changes" },
    applied: false, hardware_actuated: false, requires_explicit_acceptance: true });
  await click("Preview apply");
  expect(host.querySelector("[data-testid=apply-preview-selection]")!.textContent).toContain("profile_command");
  expect(host.textContent).toContain("Apply never changes calibration");
  expect(b.recipeCreate).not.toHaveBeenCalled();
  expect(b.recipeDerive).not.toHaveBeenCalled();
});

it("refuses a preview reply that claims to have applied, and shows backend refusals", async () => {
  await openOriginal();
  b.recipeApplyPreview.mockResolvedValue({ status: "ok", applied: true, hardware_actuated: false, requires_explicit_acceptance: true, changes: [], not_changed_by_apply: [], blocked: [], items });
  await click("Preview apply");
  expect(host.textContent).toContain("a preview must not apply or actuate");
  b.recipeDiff.mockResolvedValue({ status: "not_found", message: "Recipe r-original @ 1 not found" });
  await click("Compare with the instrument");
  expect(host.textContent).toContain("not_found: Recipe r-original @ 1 not found");
});

it("shows the provider's adoption notes as returned (#825)", async () => {
  const adopted = { ...recipe("ws-a"), adoption_notes: ["lineage not adopted: parent not in provider (ws-b @ 1)"] };
  b.recipeList.mockResolvedValue({ status: "ok", recipes: [adopted], next_after: null });
  b.recipeGet.mockResolvedValue({ status: "ok", recipe: adopted });
  act(() => root.render(<RecipeLibrary controlHeld />));
  await click("List recipes");
  await click("beads @ rev-a");
  expect(host.querySelector("[data-testid=recipe-adoption-notes]")!.textContent)
    .toBe("lineage not adopted: parent not in provider (ws-b @ 1)");
});

it("production recipe code imports no fixture", () => {
  for (const source of [librarySource, pairingSource, planSource, modelSource]) expect(source).not.toMatch(/fixtures?\//);
});
