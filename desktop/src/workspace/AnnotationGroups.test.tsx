// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, expect, it } from "vitest";
import { AnnotationGroups } from "./AnnotationGroups";
import { emptyRow, newPlanDraft } from "./planModel";

let host: HTMLDivElement, root: Root;
beforeEach(() => { Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true }); host = document.createElement("div"); document.body.append(host); root = createRoot(host); });
afterEach(() => { act(() => root.unmount()); host.remove(); });
const plan = () => ({ ...newPlanDraft("12345678-1234-4234-8234-123456789abc"), rows: [
  { ...emptyRow("S1"), condition: "Vehicle", experimentalUnit: "D1", free: [{ key: "batch", value: "A" }] },
  { ...emptyRow("S2"), condition: "Drug", experimentalUnit: "D2", free: [{ key: "batch", value: "A" }] },
] });

it("selects free-annotation grouping and exports matching rows without mutating the plan", () => {
  const draft = plan(), before = JSON.stringify(draft);
  act(() => root.render(<AnnotationGroups draft={draft} />));
  const select = host.querySelector("select")!;
  act(() => { select.value = "free:batch"; select.dispatchEvent(new Event("change", { bubbles: true })); });
  expect(host.querySelector("tbody")!.textContent).toContain("S1, S2");
  act(() => host.querySelector("button")!.click());
  const output = JSON.parse(host.querySelector("textarea")!.value);
  expect(output.selected_layer).toBe("free:batch");
  expect(output.groups[0].sample_refs).toEqual(["S1", "S2"]);
  expect(JSON.stringify(draft)).toBe(before);
});

it("clears invalid export immediately after an edit and safely falls back from a removed layer", () => {
  const draft = plan();
  act(() => root.render(<AnnotationGroups draft={draft} />));
  const select = host.querySelector("select")!;
  act(() => { select.value = "free:batch"; select.dispatchEvent(new Event("change", { bubbles: true })); });
  act(() => host.querySelector("button")!.click());
  const changed = { ...draft, rows: draft.rows.map(r => ({ ...r, free: [], concentrationValue: "1" })) };
  act(() => root.render(<AnnotationGroups draft={changed} />));
  expect(host.querySelector("select")!.value).toBe("condition");
  expect(host.querySelector("textarea")).toBeNull();
  expect(host.querySelector("button")!.disabled).toBe(true);
  expect(host.textContent).toContain("Correct the sample sheet");
});

it("renders annotation markup as text, not HTML, and never labels sample count as replicate n", () => {
  const draft = plan(); draft.rows[0].condition = "<img src=x onerror=alert(1)>";
  act(() => root.render(<AnnotationGroups draft={draft} />));
  expect(host.querySelector("img")).toBeNull();
  expect(host.textContent).toContain(draft.rows[0].condition);
  expect(host.textContent).toContain("do not establish independent replicates");
});
