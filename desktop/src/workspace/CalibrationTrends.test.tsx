// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { CalibrationTrends } from "./CalibrationTrends";
import { calRecord } from "./calibrationFixtures";

let host: HTMLDivElement, root: Root;
beforeEach(() => { Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true }); host = document.createElement("div"); document.body.append(host); root = createRoot(host); });
afterEach(() => { act(() => root.unmount()); host.remove(); });

it("shows recorded values, limits and source identity; selection is the only callback", () => {
  const select = vi.fn();
  const a = calRecord("a", "proposed"), b = calRecord("b", "insufficient", { created_at_utc: "2026-10-22T00:00:00Z" });
  act(() => root.render(<CalibrationTrends records={[b, a]} onSelect={select} />));
  expect(host.querySelectorAll("circle")).toHaveLength(2);
  expect(select).not.toHaveBeenCalled();
  act(() => host.querySelector("circle")!.dispatchEvent(new MouseEvent("click", { bubbles: true })));
  expect(select).toHaveBeenCalledWith("a");
  expect(host.textContent).toContain("run-1");
  expect(host.textContent).toContain("not current readiness");
});

it("renders missing focus without points or a fabricated zero", () => {
  act(() => root.render(<CalibrationTrends records={[calRecord("a", "proposed")]} onSelect={() => {}} />));
  const metric = host.querySelector("select")!;
  act(() => { metric.value = "focus"; metric.dispatchEvent(new Event("change", { bubbles: true })); });
  expect(host.querySelectorAll("circle")).toHaveLength(0);
  expect(host.textContent).toContain("No plottable recorded measurements");
  expect(host.textContent).toContain("not recorded");
});

it("replaces stale points when the adapter clears records; missing selection falls back", () => {
  const record = calRecord("a", "proposed");
  act(() => root.render(<CalibrationTrends records={[record]} onSelect={() => {}} />));
  const setup = host.querySelectorAll("select")[1];
  act(() => { setup.value = setup.options[1].value; setup.dispatchEvent(new Event("change", { bubbles: true })); });
  act(() => root.render(<CalibrationTrends records={[]} onSelect={() => {}} />));
  expect(host.querySelectorAll("circle")).toHaveLength(0);
  expect(host.querySelectorAll("select")[1].value).toBe("all");
});

it("provides keyboard point selection and finite coordinates for a constant history", () => {
  const select = vi.fn();
  act(() => root.render(<CalibrationTrends records={[calRecord("a", "proposed")]} onSelect={select} />));
  const point = host.querySelector("circle")!;
  expect(Number.isFinite(Number(point.getAttribute("cx")))).toBe(true);
  expect(Number.isFinite(Number(point.getAttribute("cy")))).toBe(true);
  act(() => point.dispatchEvent(new KeyboardEvent("keydown", { key: "Enter", bubbles: true })));
  expect(select).toHaveBeenCalledWith("a");
});
