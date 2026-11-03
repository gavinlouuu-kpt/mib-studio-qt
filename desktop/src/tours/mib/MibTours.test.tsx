// @vitest-environment jsdom
// MIB Studio tours: the host component. The Tours list opens and starts nothing; navigation only calls the app's view setters and
// never the bridge; action steps wait on facts the components report; the idle rule blocks them (except duringRun steps); the notice
// is quiet while anything runs, records or acquires. The generic Driver.js runner is the real one.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

// Nothing in the tours may reach the backend: any bridge or transport call fails the test.
const calls = vi.hoisted(() => ({ n: [] as string[] }));
vi.mock("../../bridge", () => ({ bridge: new Proxy({}, { get: (_t, name) => () => { calls.n.push(String(name)); } }) }));
vi.mock("../../transport", () => ({ invoke: vi.fn(async (cmd: string) => { calls.n.push(cmd); }) }));

import { facts, FACTS, reportFact, reportFactRefused } from "./facts";
import { actionBlockedReason, currentViewOf, MibTours, navigateTo, toursQuiet, type MibToursApp, type MibToursNav } from "./MibTours";
import { classicView, workspaceView } from "./views";

const IDLE: MibToursApp = { workspaceStage: 0, experimentView: "Plan", classicTab: "connect", experimentActive: false, recording: false, acquiring: false };
let host: HTMLDivElement, root: Root;
let app: MibToursApp, listOpen: boolean;
let nav: MibToursNav & { log: string[] };
const wait = async (ms: number) => act(async () => { await new Promise((r) => setTimeout(r, ms)); });
const popTitle = () => document.querySelector(".driver-popover-title")?.textContent ?? null;
const status = () => document.querySelector(".tour-action-status")?.textContent ?? null;
/** Waits (polling, with a generous deadline) until the popover shows this title. */
const until = async (title: string) => {
  for (let i = 0; i < 60 && popTitle() !== title; i++) await wait(50);
  expect(popTitle()).toBe(title);
};
/** Presses Next, then waits for the title to change (steps settle after a short delay). */
const next = async () => {
  const before = popTitle();
  await act(async () => (document.querySelector(".driver-popover-next-btn") as HTMLElement).click());
  for (let i = 0; i < 60 && popTitle() === before; i++) await wait(50);
};
/** Presses Next until the popover shows this title (never more than the entry has steps). */
const advanceTo = async (title: string) => {
  for (let i = 0; i < 16 && popTitle() !== title; i++) await next();
  expect(popTitle()).toBe(title);
};
const takeTour = async (id: string) => {
  const b = [...host.ownerDocument.querySelectorAll(`li[data-tour-entry="${id}"] button`)].find((x) => x.textContent === "Take the tour") as HTMLElement;
  expect(b, id).toBeDefined();
  await act(async () => b.click());
  for (let i = 0; i < 60 && !document.querySelector(".driver-popover-title"); i++) await wait(50);
};

/** The elements the toured views would render, by their data-tour names. */
function Page() {
  return (
    <div>
      {["connect-devices", "connect-refresh", "connect-found", "plan-start-run", "live-body", "live-stop-run"].map((n) => <button key={n} data-tour={n}>{n}</button>)}
      <MibTours listOpen={listOpen} onListClose={() => { listOpen = false; void render(); }} app={app} nav={nav} />
    </div>
  );
}
async function render() { await act(async () => root.render(<Page />)); }

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  const saved = new Map<string, string>();
  vi.stubGlobal("localStorage", { getItem: (k: string) => saved.get(k) ?? null, setItem: (k: string, v: string) => void saved.set(k, v), removeItem: (k: string) => void saved.delete(k) });
  window.matchMedia = ((q: string) => ({ matches: q.includes("reduce"), media: q, addEventListener() {}, removeEventListener() {} })) as unknown as typeof window.matchMedia;
  // jsdom lays nothing out: every element counts as shown, so the runner's default "has a box" check passes.
  vi.spyOn(Element.prototype, "getClientRects").mockReturnValue([{}] as unknown as DOMRectList);
  app = { ...IDLE }; listOpen = false; calls.n.length = 0; facts.reset();
  const log: string[] = [];
  nav = { log, showWorkspace: (s, sub) => void log.push(`workspace ${s} ${sub}`), showClassic: (t) => void log.push(`classic ${t}`) };
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => {
  await act(async () => root.unmount()); host.remove();
  vi.restoreAllMocks(); vi.unstubAllGlobals();
  expect(calls.n).toEqual([]);  // no tour, list, notice or navigation ever called the bridge
});

describe("navigation (view state only)", () => {
  it("shows workspace stages and sub-views, and the classic tabs that only read", () => {
    expect(navigateTo(workspaceView("align"), IDLE, nav)).toBe("workspace");
    expect(navigateTo(workspaceView("experiment", "run-summary"), IDLE, nav)).toBe("workspace");
    expect(navigateTo(classicView("review"), IDLE, nav)).toBe("classic");
    expect(navigateTo(classicView("connect"), { ...IDLE, workspaceStage: null, classicTab: "review" }, nav)).toBe("classic");
    expect(nav.log).toEqual(["workspace 1 null", "workspace 2 run-summary", "classic review", "classic connect"]);
  });
  it("never moves to a classic tab that can act (Overview, Experiment), unless it is the tab already shown", () => {
    const classicConnect = { ...IDLE, workspaceStage: null as number | null };
    expect(navigateTo(classicView("overview"), classicConnect, nav)).toBe("ignored");
    expect(navigateTo(classicView("experiment"), classicConnect, nav)).toBe("ignored");
    expect(nav.log).toEqual([]);
    // Restoring a start view that was the shown classic tab only hides the workspace again.
    expect(navigateTo(classicView("overview"), { ...classicConnect, classicTab: "overview" }, nav)).toBe("classic");
    expect(nav.log).toEqual(["classic overview"]);
  });
  it("ignores a view it does not know", () => {
    expect(navigateTo({ shell: "tablet" }, IDLE, nav)).toBe("ignored");
    expect(navigateTo({ shell: "workspace", stage: "somewhere" }, IDLE, nav)).toBe("ignored");
    expect(nav.log).toEqual([]);
  });
  it("reads the current view back whole, including the Experiment sub-view", () => {
    expect(currentViewOf({ ...IDLE, workspaceStage: 2, experimentView: "Run summary" })).toEqual(workspaceView("experiment", "run-summary"));
    expect(currentViewOf({ ...IDLE, workspaceStage: 0, experimentView: "Live" })).toEqual(workspaceView("prepare", "live"));
    expect(currentViewOf({ ...IDLE, workspaceStage: null, classicTab: "review" })).toEqual(classicView("review"));
  });
});

describe("the idle rule", () => {
  it("blocks action steps while an experiment runs, something records or the camera acquires", () => {
    expect(actionBlockedReason(IDLE)).toBeNull();
    expect(actionBlockedReason({ ...IDLE, experimentActive: true })).toBe("an experiment is running");
    expect(actionBlockedReason({ ...IDLE, recording: true })).toBe("a recording is in progress");
    expect(actionBlockedReason({ ...IDLE, acquiring: true })).toBe("the camera is acquiring");
    expect(toursQuiet(IDLE)).toBe(false);
    for (const busy of [{ experimentActive: true }, { recording: true }, { acquiring: true }]) expect(toursQuiet({ ...IDLE, ...busy })).toBe(true);
  });

  it("a step that needs the camera (a feature write) is not blocked by the camera acquiring, only by a run or a recording", () => {
    const needs = { needsCamera: true as const };
    expect(actionBlockedReason({ ...IDLE, acquiring: true }, needs)).toBeNull();
    expect(actionBlockedReason({ ...IDLE, acquiring: true, experimentActive: true }, needs)).toBe("an experiment is running");
    expect(actionBlockedReason({ ...IDLE, acquiring: true, recording: true }, needs)).toBe("a recording is in progress");
    expect(actionBlockedReason({ ...IDLE, acquiring: true }, {})).toBe("the camera is acquiring");   // every other action step is still blocked
  });
});

describe("the list and the notice", () => {
  it("the list opens on request with the six tours and starts none", async () => {
    listOpen = true; await render(); await wait(50);
    const list = host.ownerDocument.querySelector('[aria-label="Tours and what\'s new"]')!;
    expect(list).not.toBeNull();
    expect([...list.querySelectorAll("li[data-tour-entry]")].map((l) => l.getAttribute("data-tour-entry"))).toEqual([
      "fold-in-e01-discovery", "fold-in-e02-features", "fold-in-e04-e06-runs", "fold-in-i02-recipes", "fold-in-f04-population", "fold-in-i12-f12-workspace",
    ]);
    await wait(250);
    expect(document.querySelector(".driver-popover")).toBeNull();
    expect(nav.log).toEqual([]);
  });

  it("the notice counts the unseen tours, never starts one, and is remembered once dismissed", async () => {
    await render();
    const notice = () => document.querySelector('[data-testid="tour-notice"]');
    expect(notice()?.textContent).toBe("6 new: take the tour×");
    await wait(250);
    expect(document.querySelector(".driver-popover")).toBeNull();
    await act(async () => (document.querySelector(".tour-notice-dismiss") as HTMLElement).click());
    expect(notice()).toBeNull();
    expect(JSON.parse(localStorage.getItem("yofo.studio.tours.seen")!).seen).toHaveLength(6);
  });

  it("the notice stays away while an experiment is Starting or Active, anything records, or the camera acquires", async () => {
    for (const busy of [{ experimentActive: true }, { recording: true }, { acquiring: true }]) {
      app = { ...IDLE, ...busy };
      await render();
      expect(document.querySelector('[data-testid="tour-notice"]'), JSON.stringify(busy)).toBeNull();
    }
    app = { ...IDLE }; await render();
    expect(document.querySelector('[data-testid="tour-notice"]')).not.toBeNull();
  });
});

describe("the discovery tour end to end (the real runner)", () => {
  async function toRefreshStep() {
    listOpen = true; await render();
    await takeTour("fold-in-e01-discovery");
    await until("Before: a job ID named the scan");
    await advanceTo("Available devices");                          // 4 flow steps and the evidence panel first
    expect(nav.log).toContain("classic connect");                  // the step's view: the classic Connect tab
    await next();
    await until("Press Refresh");
  }

  it("waits for the backend's finished job, shows its instance, and restores the start view", async () => {
    await toRefreshStep();
    const nextBtn = () => document.querySelector(".driver-popover-next-btn") as HTMLButtonElement;
    expect(nextBtn().disabled).toBe(true);
    expect(status()).toBe("Press Refresh. Waiting for the backend to confirm…");
    reportFact(FACTS.discovery, "job 1, instance c9b79788");
    await wait(400);
    expect(status()).toBe("Done: the backend finished a discovery job (job 1, instance c9b79788).");
    expect(nextBtn().disabled).toBe(false);
    await next();
    await until("What the job found");
    await next();
    await until("Was this tour clear?");
    await next();
    for (let i = 0; i < 40 && popTitle() !== null; i++) await wait(50);
    expect(popTitle()).toBeNull();
    expect(nav.log[nav.log.length - 1]).toBe("workspace 0 plan");     // back where the tour started
  });

  it("a stale reply is shown as what happened, with the backend's words", async () => {
    await toRefreshStep();
    reportFactRefused(FACTS.discovery, "Error: discovery job 7 is stale: the backend restarted (instance x); scan again");
    await wait(400);
    expect(popTitle()).toBe("What happened: Press Refresh");
    expect(document.querySelector(".driver-popover-description")?.textContent).toContain("is stale: the backend restarted");
  });

  it("is blocked while the camera acquires: the step says so and waits for nothing", async () => {
    app = { ...IDLE, acquiring: true };
    await toRefreshStep();
    expect(status()).toBe("Not now: the camera is acquiring. Skip this step to go on.");
    expect((document.querySelector(".driver-popover-next-btn") as HTMLButtonElement).disabled).toBe(true);
    expect(document.querySelector('[data-tour="connect-refresh"]')!.classList.contains("driver-no-interaction")).toBe(true);
  });

  it("facts reported before the tour started do not count: the tour waits for what you do during it", async () => {
    reportFact(FACTS.discovery, "job 0, instance old");
    await toRefreshStep();
    expect(status()).toBe("Press Refresh. Waiting for the backend to confirm…");
  });
});

describe("the runs tour: the run steps work while the run is active", () => {
  it("the Start, first-frame and Stop steps are not blocked by the run they start (duringRun)", async () => {
    listOpen = true; await render();
    await takeTour("fold-in-e04-e06-runs");
    await advanceTo("Start a run");                                // 4 flow steps and the evidence panel first
    expect(status()).toBe("Press Start run. Waiting for the backend to confirm…");
    app = { ...IDLE, experimentActive: true }; await render();     // the run is now Starting or Active
    reportFact(FACTS.runStart, "run generation 3 · /data/run.h5");
    await wait(400);
    expect(status()).toBe("Done: the backend accepted the start (run generation 3 · /data/run.h5). That is not yet a first frame.");
    await next();
    await until("Wait for the first frame");
    expect(status()).toBe("Nothing to press; watch the Live view. Waiting for the backend to confirm…");  // start accepted does not satisfy it
    reportFact(FACTS.runFirstFrame, "run generation 3 · 12 valid and 0 invalid frames buffered");
    await wait(400);
    expect(status()).toContain("Done: the run's status showed frames");
    await next();
    await until("Stop and finalize");
    expect(status()).toBe("Press Stop & finalize run. Waiting for the backend to confirm…");
    expect(nav.log).toContain("workspace 2 live");
  });
});
