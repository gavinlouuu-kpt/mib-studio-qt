// @vitest-environment jsdom
// Guided tours, generic module: the optional host hooks added in the MIB Studio port (duringRun, {detail}, reveal, confirmedBy,
// onTourStart). Same page as runner.test.tsx: the real Driver.js on a small page, no app, no instrument.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { TourEntry, TourView } from "./registry";
import type { ConditionState } from "./runner";
import { TourHost } from "./TourHost";

const act1 = (action: Partial<NonNullable<TourEntry["steps"][number]["action"]>> = {}) => ({
  await: "done", prompt: "Press the button.", success: "the host confirmed it ({detail}).", recovery: "Check the log.", timeoutMs: 600, ...action,
});
const TOUR = (action = act1(), id = "hooks", target = "ctl"): TourEntry => ({
  id, title: "Hooks", summary: "s", kind: "feature", introduced: { date: "2026-11-03", by: "t" }, source: ["d.md"], when: () => true,
  steps: [{ target: { ui: target }, title: "Do it", text: "Use the control.", cite: ["d.md"], action }],
});

let host: HTMLDivElement, root: Root;
let cond: ConditionState, blocked: string | null, listOpen: boolean, detail: string | undefined;
let started: string[], revealed: Element[];
const wait = async (ms: number) => act(async () => { await new Promise((r) => setTimeout(r, ms)); });
const popTitle = () => document.querySelector(".driver-popover-title")?.textContent ?? null;
const status = () => document.querySelector(".tour-action-status")?.textContent ?? null;
const btn = async (label: string) => { const b = [...document.querySelectorAll("button")].find((x) => x.textContent === label); expect(b, label).toBeDefined(); await act(async () => b!.click()); await wait(120); };

function Page({ entry }: { entry: TourEntry }) {
  const view: TourView = {};
  return (
    <div>
      <button data-tour="ctl">ctl</button>
      <details data-testid="fold"><summary data-tour="fold-summary">More</summary><button data-tour="inside">inside</button></details>
      <TourHost entries={[entry]} storageKey="test.tours" quiet={false} listOpen={listOpen} onListClose={() => { listOpen = false; }}
        navigate={() => {}} currentView={() => view} conditions={{ done: () => cond }} actionBlocked={() => blocked}
        onTourStart={(e) => started.push(e.id)} detail={() => detail}
        reveal={(el) => { revealed.push(el); el.closest("details")?.setAttribute("open", ""); }} confirmedBy="the backend"
        runnerOverrides={{ isShown: (el) => el.isConnected && !el.closest("details:not([open])") , settle: () => new Promise((r) => setTimeout(r, 20)), pollMs: 50 }} />
    </div>
  );
}
const render = async (entry: TourEntry) => act(async () => root.render(<Page entry={entry} />));

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  const saved = new Map<string, string>();
  vi.stubGlobal("localStorage", { getItem: (k: string) => saved.get(k) ?? null, setItem: (k: string, v: string) => void saved.set(k, v), removeItem: (k: string) => void saved.delete(k) });
  window.matchMedia = ((q: string) => ({ matches: q.includes("reduce"), media: q, addEventListener() {}, removeEventListener() {} })) as unknown as typeof window.matchMedia;
  cond = "pending"; blocked = null; listOpen = true; detail = undefined; started = []; revealed = [];
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => {
  await act(async () => root.unmount()); host.remove();
  vi.unstubAllGlobals();
});

describe("host hooks of the generic module", () => {
  it("onTourStart runs once, when a tour is taken, before its first step", async () => {
    await render(TOUR());
    expect(started).toEqual([]);
    await btn("Take the tour"); await wait(150);
    expect(started).toEqual(["hooks"]);
    expect(popTitle()).toBe("Do it");
  });

  it("confirmedBy names who confirms; the default stays 'the board'", async () => {
    await render(TOUR());
    await btn("Take the tour"); await wait(150);
    expect(status()).toBe("Press the button. Waiting for the backend to confirm…");
  });

  it("{detail} in the success text is the host's formatted detail, or says there is none", async () => {
    detail = "instance abc";
    await render(TOUR());
    await btn("Take the tour"); await wait(150);
    cond = "met"; await wait(150);
    expect(status()).toBe("Done: the host confirmed it (instance abc).");
    await btn("Skip");
    await act(async () => { window.dispatchEvent(new KeyboardEvent("keyup", { key: "Escape" })); }); await wait(150);
    detail = undefined; listOpen = true;
    await render(TOUR());
    await btn("Take the tour"); await wait(150);
    expect(status()).toBe("Already done: the host confirmed it (no detail reported).");
  });

  it("an action step marked duringRun ignores actionBlocked, at the start and while it waits", async () => {
    blocked = "an experiment is running";
    await render(TOUR(act1({ duringRun: true })));
    await btn("Take the tour"); await wait(150);
    expect(status()).toBe("Press the button. Waiting for the backend to confirm…");  // not "Not now"
    expect(document.querySelector('[data-tour="ctl"]')!.classList.contains("driver-no-interaction")).toBe(false);
    await wait(200);
    expect(popTitle()).toBe("Do it");  // no "Stopped waiting" recovery
    cond = "met"; await wait(150);
    expect(status()).toContain("Done:");
    expect((document.querySelector(".driver-popover-next-btn") as HTMLButtonElement).disabled).toBe(false);
  });

  it("without duringRun the same blocked state stops the step", async () => {
    blocked = "an experiment is running";
    await render(TOUR());
    await btn("Take the tour"); await wait(150);
    expect(status()).toBe("Not now: an experiment is running. Skip this step to go on.");
  });

  it("reveal is asked for a step's element and may open the section that holds it", async () => {
    await render(TOUR(act1(), "reveal", "inside"));
    await btn("Take the tour"); await wait(150);
    expect(revealed.map((e) => e.getAttribute("data-tour"))).toEqual(["inside"]);
    expect(popTitle()).toBe("Do it");  // shown, because the host opened the <details>
    expect(document.querySelector('[data-tour="inside"]')!.classList.contains("driver-active-element")).toBe(true);
  });
});
