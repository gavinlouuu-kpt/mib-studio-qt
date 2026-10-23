// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { beforeEach, afterEach, expect, it, vi } from "vitest";
import { PopulationAnalysis } from "./PopulationAnalysis";
import { reviewBridge } from "./reviewBridge";
vi.mock("./reviewBridge", () => ({ reviewBridge: { analyzePopulation: vi.fn() } }));
Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let root: Root, host: HTMLDivElement;
const reply = {ok: true, schema: "review-population/1", path: "a.h5", feature: "area", selected: "14", excluded: "0", unknown: "0", valid: "10", invalid: "4", measured: "14", missing: "0", minimum: 1, maximum: 2, mean: 1.5, median: 1.5, unit: "px²"};
beforeEach(async () => {
  vi.resetAllMocks();
  const storage = new Map<string, string>();
  Object.defineProperty(window, "localStorage", { configurable: true, value: {
    getItem: (key: string) => storage.get(key) ?? null,
    setItem: (key: string, value: string) => storage.set(key, value),
    clear: () => storage.clear(),
  }});
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
  await act(async () => root.render(<PopulationAnalysis path="a.h5" disabled={false} />));
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });
it("calls the backend and renders both-set statistics, not client calculations", async () => {
  vi.mocked(reviewBridge.analyzePopulation).mockResolvedValue(reply as never);
  await act(async () => host.querySelector("button")!.click());
  expect(reviewBridge.analyzePopulation).toHaveBeenCalledWith({expectedPath: "a.h5", feature: "area", minimum: null, maximum: null});
  expect(host.textContent).toContain("14 selected");
  expect(host.textContent).toContain("10 valid + 4 invalid");
});
it("shows a backend refusal instead of stale or zero results", async () => {
  vi.mocked(reviewBridge.analyzePopulation).mockResolvedValue({ok: false, message: "Incomplete source"} as never);
  await act(async () => host.querySelector("button")!.click());
  expect(host.querySelector('[role="alert"]')?.textContent).toContain("Incomplete source");
  expect(host.querySelector("table")).toBeNull();
});
it("discards a late result after source replacement", async () => {
  let finish!: (value: never) => void;
  vi.mocked(reviewBridge.analyzePopulation).mockImplementation(() => new Promise(resolve => { finish = resolve; }));
  await act(async () => host.querySelector("button")!.click());
  await act(async () => root.render(<PopulationAnalysis path="b.h5" disabled={false} />));
  await act(async () => finish(reply as never));
  expect(host.querySelector("table")).toBeNull();
});
it("disables analysis while a review job owns the source lane", async () => {
  await act(async () => root.render(<PopulationAnalysis path="a.h5" disabled />));
  expect(host.querySelector("fieldset")?.disabled).toBe(true);
});
it("persists a recipe across remount and sends it to native graph evaluation", async () => {
  window.localStorage.clear();
  const click = async (label: string) => act(async () => Array.from(host.querySelectorAll('button')).find(b => b.textContent === label)!.click());
  await click("Save recipe");
  expect(window.localStorage.getItem("mib.analysis.graph.recipe.v1")).toContain("area-gate");
  await act(async () => root.render(<PopulationAnalysis key="reopened" path="a.h5" disabled={false} />));
  await click("Restore recipe");
  vi.mocked(reviewBridge.analyzePopulation).mockResolvedValue(reply as never);
  await click("Analyze population");
  expect(vi.mocked(reviewBridge.analyzePopulation).mock.calls.slice(-1)[0]?.[0].graph?.output).toBe("area-gate");
});
it("requests a bounded selected page and preserves unavailable-image identity", async () => {
  vi.mocked(reviewBridge.analyzePopulation).mockResolvedValue({...reply, galleryOffset: 0, gallery: [{key: "snapshot:invalid:2", set: "invalid", ordinal: "2", pixels: null}]} as never);
  await act(async () => host.querySelector("button")!.click());
  await act(async () => Array.from(host.querySelectorAll('button')).find(b => b.textContent === "Load gated gallery")!.click());
  expect(vi.mocked(reviewBridge.analyzePopulation).mock.calls.slice(-1)[0]?.[0].galleryOffset).toBe(0);
  expect(host.textContent).toContain("Image unavailable");
  expect(host.textContent).toContain("invalid · row 2");
});

it("reports a storage failure without claiming the recipe was saved", async () => {
  vi.spyOn(window.localStorage, "setItem").mockImplementation(() => { throw new Error("Storage quota exceeded"); });
  await act(async () => Array.from(host.querySelectorAll('button')).find(b => b.textContent === "Save recipe")!.click());
  expect(host.querySelector('[role="alert"]')?.textContent).toContain("Storage quota exceeded");
  expect(host.textContent).not.toContain("Recipe saved in this browser");
});
