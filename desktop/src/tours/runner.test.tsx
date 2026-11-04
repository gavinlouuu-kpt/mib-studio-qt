// @vitest-environment jsdom
// Guided tours, generic module: TourHost and the runner with the real Driver.js, on a small page (no app, no instrument).
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { FlowSpec, TourEntry, TourView } from "./registry";
import type { ConditionState } from "./runner";
import { TourHost } from "./TourHost";

const FLOW: FlowSpec = { title: "Chart", cols: 2, nodes: [{ id: "a", label: "A", col: 0, row: 0 }, { id: "b", label: "B", col: 1, row: 1 }], edges: [{ from: "a", to: "b" }] };
const UI_TOUR: TourEntry = {
  id: "ui", title: "UI tour", summary: "s", kind: "feature", introduced: { date: "2026-11-01", by: "t" }, source: ["d.md"], when: () => true,
  steps: [
    { target: { ui: "here" }, title: "Here", text: "On the page.", cite: ["d.md"] },
    { target: { ui: "absent" }, title: "Gone", text: "Never rendered.", cite: ["d.md"] },
    { target: { ui: "other", view: { tab: "two" } }, title: "Other view", text: "After a view change.", cite: ["d.md"] },
  ],
};
const ACTION_TOUR: TourEntry = {
  id: "act", title: "Action tour", summary: "s", kind: "feature", introduced: { date: "2026-11-02", by: "t" }, source: ["d.md"], when: () => true,
  steps: [{ target: { ui: "ctl" }, title: "Press it", text: "Use the control.", cite: ["d.md"],
    action: { await: "done", prompt: "Press the button.", success: "the board confirmed it.", recovery: "Check the log.", timeoutMs: 600 } }],
};
const INTERACTIVE_TOUR: TourEntry = {
  id: "int", title: "Interactive tour", summary: "s", kind: "feature", introduced: { date: "2026-11-02", by: "t" }, source: ["d.md"], when: () => true,
  steps: [
    { target: { ui: "ctl" }, title: "Press it first", text: "Press the control, then Next.", cite: ["d.md"], interactive: true },
    { target: { ui: "here" }, title: "Only pointed at", text: "Nothing to press.", cite: ["d.md"] },
  ],
};
const NEEDS_CAMERA_TOUR: TourEntry = {
  ...ACTION_TOUR, id: "cam",
  steps: [{ ...ACTION_TOUR.steps[0], action: { ...ACTION_TOUR.steps[0].action!, needsCamera: true } }],
};
const FLOW_TOUR: TourEntry = {
  id: "flow", title: "Flow tour", summary: "s", kind: "backend", introduced: { date: "2026-10-30", by: "t" }, source: ["d.md"], when: () => true,
  flows: { before: FLOW, after: FLOW }, evidence: { demonstrated: [], tested: [], uncertain: [{ text: "open", source: "d.md" }] },
  steps: [
    { target: { flow: "before", node: "a" }, title: "Before A", text: "a", cite: ["d.md"] },
    { target: { flow: "after", edge: "a->b" }, title: "After edge", text: "e", cite: ["d.md"] },
    { target: { evidence: true }, title: "Evidence", text: "ev", cite: ["d.md"] },
    { target: { ui: "here" }, title: "Where it shows", text: "ui", cite: ["d.md"] },
  ],
};

let host: HTMLDivElement, root: Root;
let view: TourView, cond: ConditionState, blocked: string | null, listOpen: boolean;
const navigations: TourView[] = [];
const asked: unknown[] = [];   // what actionBlocked was asked about, by step
const wait = async (ms: number) => act(async () => { await new Promise((r) => setTimeout(r, ms)); });
const popTitle = () => document.querySelector(".driver-popover-title")?.textContent ?? null;
const popText = () => document.querySelector(".driver-popover-description")?.textContent ?? "";
const press = async (sel: string) => { const b = document.querySelector(sel) as HTMLElement | null; expect(b, sel).not.toBeNull(); await act(async () => b!.click()); await wait(120); };
const btn = async (label: string) => { const b = [...document.querySelectorAll("button")].find((x) => x.textContent === label); expect(b, label).toBeDefined(); await act(async () => b!.click()); await wait(120); };

function Page({ entries, quiet = false }: { entries: TourEntry[]; quiet?: boolean }) {
  return (
    <div>
      <div data-tour="here">here</div>
      {view.tab === "two" && <div data-tour="other">other</div>}
      <button data-tour="ctl">ctl</button>
      <TourHost entries={entries} storageKey="test.tours" quiet={quiet} listOpen={listOpen} onListClose={() => { listOpen = false; }}
        navigate={(v) => { navigations.push(v); view = v; void render(entries, quiet); }} currentView={() => view}
        conditions={{ done: () => cond }} actionBlocked={(action) => { asked.push(action); return blocked; }}
        runnerOverrides={{ isShown: (el) => el.isConnected, settle: () => new Promise((r) => setTimeout(r, 20)), pollMs: 50 }} />
    </div>
  );
}
async function render(entries: TourEntry[], quiet = false) { await act(async () => root.render(<Page entries={entries} quiet={quiet} />)); }

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  const saved = new Map<string, string>();
  vi.stubGlobal("localStorage", { getItem: (k: string) => saved.get(k) ?? null, setItem: (k: string, v: string) => void saved.set(k, v), removeItem: (k: string) => void saved.delete(k) });
  window.matchMedia = ((q: string) => ({ matches: q.includes("reduce"), media: q, addEventListener() {}, removeEventListener() {} })) as unknown as typeof window.matchMedia;
  view = { tab: "one" }; cond = "pending"; blocked = null; listOpen = false; navigations.length = 0; asked.length = 0;
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => {
  await act(async () => root.unmount()); host.remove();
  expect(document.querySelector(".driver-popover")).toBeNull();  // unmounting ended any tour
  vi.unstubAllGlobals();
});

describe("TourHost and the runner", () => {
  it("the notice counts unseen tours and never starts one; dismissing it remembers", async () => {
    await render([UI_TOUR, ACTION_TOUR]);
    expect(document.querySelector('[data-testid="tour-notice"]')?.textContent).toBe("2 new: take the tour×");
    expect(popTitle()).toBeNull();
    await press(".tour-notice-dismiss");
    expect(document.querySelector('[data-testid="tour-notice"]')).toBeNull();
    expect(JSON.parse(localStorage.getItem("test.tours.seen")!)).toEqual({ v: 1, seen: ["act", "ui"] });
    await act(async () => root.unmount()); root = createRoot(host);
    await render([UI_TOUR, ACTION_TOUR]);
    expect(document.querySelector('[data-testid="tour-notice"]')).toBeNull();
  });

  it("quiet hides the notice (an experiment or a recording)", async () => {
    await render([UI_TOUR], true);
    expect(document.querySelector('[data-testid="tour-notice"]')).toBeNull();
  });

  it("take the tour with several new opens the list, with badges, and marks them seen", async () => {
    await render([UI_TOUR, ACTION_TOUR]);
    await press(".tour-notice-take");
    const list = document.querySelector('[aria-label="Tours and what\'s new"]');
    expect(list).not.toBeNull();
    expect([...list!.querySelectorAll("li.new")].map((l) => l.getAttribute("data-tour-entry"))).toEqual(["ui", "act"]);
    expect(JSON.parse(localStorage.getItem("test.tours.seen")!).seen).toEqual(["act", "ui"]);
    expect(popTitle()).toBeNull();
  });

  it("steps through, recovers from a missing target, changes view, and ends with feedback", async () => {
    listOpen = true;
    await render([UI_TOUR]);
    await btn("Take the tour");
    await wait(150);
    expect(popTitle()).toBe("Here");
    expect(document.querySelector('[data-tour="here"]')!.classList.contains("driver-active-element")).toBe(true);
    for (const label of ["Skip", "← Back", "Next →"]) expect([...document.querySelectorAll(".driver-popover button")].map((b) => b.textContent)).toContain(label);
    await press(".driver-popover-next-btn");
    expect(popTitle()).toBe("Not on screen: Gone");
    expect(popText()).toContain("Skip this step, or restart the tour.");
    expect(document.querySelector(".driver-active-element")?.id ?? "").toBe("driver-dummy-element");
    await press(".tour-skip");
    expect(navigations).toContainEqual({ tab: "two" });
    expect(popTitle()).toBe("Other view");
    await press(".tour-restart");
    expect(popTitle()).toBe("Here");
    for (let i = 0; i < 3; i++) await press(".driver-popover-next-btn");
    expect(popTitle()).toBe("Was this tour clear?");
    await press(".tour-verdict-unclear");
    (document.querySelector(".tour-feedback-note") as HTMLTextAreaElement).value = "step 2 missing";
    await press(".driver-popover-next-btn");
    expect(popTitle()).toBeNull();
    expect(JSON.parse(localStorage.getItem("test.tours.feedback")!)).toMatchObject([{ tour: "ui", verdict: "unclear", note: "step 2 missing", tourVersion: "2026-11-01" }]);
    expect(navigations[navigations.length - 1]).toEqual({ tab: "one" });  // back where it started
  });

  it("Esc ends the tour; arrow keys move it", async () => {
    listOpen = true;
    await render([UI_TOUR]);
    await btn("Take the tour"); await wait(150);
    await act(async () => { window.dispatchEvent(new KeyboardEvent("keyup", { key: "ArrowRight" })); }); await wait(150);
    expect(popTitle()).toBe("Not on screen: Gone");
    await act(async () => { window.dispatchEvent(new KeyboardEvent("keyup", { key: "ArrowLeft" })); }); await wait(150);
    expect(popTitle()).toBe("Here");
    await act(async () => { window.dispatchEvent(new KeyboardEvent("keyup", { key: "Escape" })); }); await wait(150);
    expect(popTitle()).toBeNull();
    expect(document.body.classList.contains("driver-active")).toBe(false);
  });

  it("an action step waits for the host's confirmation, then allows Next", async () => {
    listOpen = true;
    await render([ACTION_TOUR]);
    await btn("Take the tour"); await wait(150);
    expect(popTitle()).toBe("Press it");
    const next = document.querySelector(".driver-popover-next-btn") as HTMLButtonElement;
    expect(next.disabled).toBe(true);
    expect(document.querySelector(".tour-action-status")?.textContent).toBe("Press the button. Waiting for the board to confirm…");
    expect(document.querySelector('[data-tour="ctl"]')!.classList.contains("driver-no-interaction")).toBe(false);  // the real control works
    await act(async () => { window.dispatchEvent(new KeyboardEvent("keyup", { key: "ArrowRight" })); }); await wait(60);
    expect(popTitle()).toBe("Press it");  // no advance before the confirmation
    cond = "met";
    await wait(150);
    expect(document.querySelector(".tour-action-status")?.textContent).toBe("Done: the board confirmed it.");
    expect((document.querySelector(".driver-popover-next-btn") as HTMLButtonElement).disabled).toBe(false);
    await press(".driver-popover-next-btn");
    expect(popTitle()).toBe("Was this tour clear?");
  });

  it("an action step that fails or times out shows what happened, with Try again", async () => {
    listOpen = true;
    await render([ACTION_TOUR]);
    await btn("Take the tour"); await wait(150);
    cond = { failed: "Camera: faulted (camera)." };
    await wait(150);
    expect(popTitle()).toBe("What happened: Press it");
    expect(popText()).toBe("Camera: faulted (camera). Check the log.");
    cond = "pending";
    await btn("Try again");
    expect(popTitle()).toBe("Press it");
    await wait(800);
    expect(popTitle()).toBe("What happened: Press it");
    expect(popText()).toBe("No confirmation within 1 s. Check the log.");
  });

  it("an action step is disabled while the host blocks actions", async () => {
    blocked = "an experiment is running";
    listOpen = true;
    await render([ACTION_TOUR]);
    await btn("Take the tour"); await wait(150);
    expect(document.querySelector(".tour-action-status")?.textContent).toBe("Not now: an experiment is running. Skip this step to go on.");
    expect(document.querySelector('[data-tour="ctl"]')!.classList.contains("driver-no-interaction")).toBe(true);
    expect((document.querySelector(".driver-popover-next-btn") as HTMLButtonElement).disabled).toBe(true);
  });

  it("the host is told the step's action when it is asked whether the step is blocked (needsCamera)", async () => {
    listOpen = true;
    await render([ACTION_TOUR, NEEDS_CAMERA_TOUR]);
    await btn("Take the tour"); await wait(150);
    expect(asked.length).toBeGreaterThan(0);   // at the step, and again at each poll while it waits
    for (const a of asked) { expect(a).toBe(ACTION_TOUR.steps[0].action); expect(a).not.toHaveProperty("needsCamera"); }
    await press(".driver-popover-close-btn");
    asked.length = 0; listOpen = true;
    await render([ACTION_TOUR, NEEDS_CAMERA_TOUR]);
    const takes = [...document.querySelectorAll("button")].filter((x) => x.textContent === "Take the tour");
    await act(async () => takes[1].click()); await wait(150);
    expect(popTitle()).toBe("Press it");
    expect(asked.length).toBeGreaterThan(0);
    for (const a of asked) expect(a).toMatchObject({ needsCamera: true });
  });

  it("a step marked interactive leaves its control usable and waits for nothing; a plain step does not", async () => {
    listOpen = true;
    await render([INTERACTIVE_TOUR]);
    await btn("Take the tour"); await wait(150);
    expect(popTitle()).toBe("Press it first");
    expect(document.querySelector('[data-tour="ctl"]')!.classList.contains("driver-no-interaction")).toBe(false);  // the real control works
    expect(document.querySelector(".tour-action-status")).toBeNull();                                            // not an action step: nothing to wait for
    expect((document.querySelector(".driver-popover-next-btn") as HTMLButtonElement).disabled).toBe(false);
    expect(asked).toEqual([]);                                                                                    // and the idle rule is not consulted
    await press(".driver-popover-next-btn");
    expect(popTitle()).toBe("Only pointed at");
    expect(document.querySelector('[data-tour="here"]')!.classList.contains("driver-no-interaction")).toBe(true);
  });

  it("flow steps open the before/after panel and highlight nodes, edges and the evidence, then the UI", async () => {
    listOpen = true;
    await render([FLOW_TOUR]);
    await btn("Take the tour"); await wait(150);
    expect(document.querySelector('[role="dialog"][aria-label="Flow tour"]')).not.toBeNull();
    expect(document.querySelector('[data-tour="flow:before:a"]')!.classList.contains("driver-active-element")).toBe(true);
    await press(".driver-popover-next-btn");
    expect(document.querySelector('[data-tour="flow:after:a->b"]')!.classList.contains("driver-active-element")).toBe(true);
    await press(".driver-popover-next-btn");
    expect(popTitle()).toBe("Evidence");
    expect(document.querySelector('[data-tour="flow:evidence"]')!.textContent).toContain("None recorded in the sources.");
    await press(".driver-popover-next-btn");
    expect(document.querySelector('[role="dialog"][aria-label="Flow tour"]')).toBeNull();
    expect(popTitle()).toBe("Where it shows");
  });
});
