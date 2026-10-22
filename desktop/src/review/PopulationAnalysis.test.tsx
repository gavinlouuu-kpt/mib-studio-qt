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
  vi.resetAllMocks(); host = document.createElement("div"); document.body.append(host); root = createRoot(host);
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
