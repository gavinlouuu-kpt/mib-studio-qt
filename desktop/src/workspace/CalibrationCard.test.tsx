// @vitest-environment jsdom
// Prepare's Calibration card (#716 A4b) over the mocked transport: the gate and its Fix, the record
// in effect, proposed and insufficient records, the plain list, and the one explicit action.
// Every IPC call is counted: showing, listing, selecting and polling never send calibration_apply;
// Apply / Re-apply sends exactly one, with the factor the page last read; a refusal changes nothing
// on screen but the re-read; a viewer, or a run, sends nothing.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { invoke } from "@tauri-apps/api/core";
import { CALIBRATION_APPLY_OUTCOMES as O, JSON_TRANSPORT, READINESS_GATE_STATUSES as G } from "../bridgeContract";
import type { ReadinessGate } from "../eventAdapter";
import type { PreflightInput } from "../preflight";
import { applyReply, calList, calRecord, SETUP } from "./calibrationFixtures";
import { PREPARE_READINESS_POLL_MS } from "./prepareModel";
import { Workspace, type WorkspaceProps } from "./Workspace";
import type { AlignFacts } from "./alignControls";
import { newPlanDraft } from "./planModel";
import { newQueue } from "./queueModel";
import type { PlanController } from "./usePlan";

vi.mock("@tauri-apps/api/core", () => ({ invoke: vi.fn() }));

const OFF = { valid: true, connected: false, identity: "" };
const INPUT: PreflightInput = {
  backendReady: true, cameraConfigured: true, cameraRunning: false, cameraIdentity: "Mock camera", cameraExpected: "",
  coreKnown: true, coreValid: true, corePinSatisfied: true, coreVersion: "2.1.0", requiredCoreVersion: "2.1.0",
  autofocus: OFF, samplePump: OFF, sheathPump: OFF, trigger: { valid: true, cameraAttached: false },
  storageKnown: false, storageWritable: false, storageFreeOk: false, storagePath: "",
};
const FACTS: AlignFacts = {
  pz7035: false, instrumentModes: false, hostProcessing: true, geometry: null, instrumentMode: null, runFrozen: false, modeError: "",
  appliedRunWindow: null, placedRunWindow: null, processingRoi: null, runFps: null, experimentActive: false, running: false,
};
const PLAN: PlanController = {
  draft: newPlanDraft("3f2b8c1e-9a4d-4e6f-8b21-0c5d7e9f1a2b"), edit: () => {}, operators: [], result: null, started: null, starting: false,
  start: async () => null, newPlan: () => {},
  queue: { state: newQueue(), pending: null, controlHeld: true, status: null, startCurrent: async () => {}, stopCurrent: async () => {},
    confirmClosed: () => {}, skip: () => {}, end: () => {}, resume: () => {} },
};
const props = (over: Partial<WorkspaceProps> = {}): WorkspaceProps => ({
  brand: "MIB Studio", source: "Mock camera", method: "", stage: 0, onStage: () => {}, onExit: () => {}, serviceMode: false,
  preflightInput: INPUT, zStage: null, signature: "sig", confirmedFor: "", onConfirm: () => {}, confirmLockedReason: "",
  experimentActive: false, controlHeld: true, onRefreshChecks: () => {}, onOpenSetup: () => {},
  align: { facts: FACTS, subscribeFrames: () => {}, onSwitchMode: async () => {}, writeRoi: vi.fn(), lockedReason: "" },
  experiment: { plan: PLAN, lockedReason: "", view: "Plan", onView: () => {}, live: { snapshot: null, running: false, experimentActive: false, hostProcessing: true,
    subscribeFrames: () => {}, onStopAcquisition: async () => {}, onStopRun: async () => ({ ok: false, message: "not in this test" }) } },
  ...over,
});

const NO_PATH: ReadinessGate = { id: "storage.output", status: G.Unavailable, reason: "no output path chosen yet", remediation: "choose an HDF5 destination" };
let calGate: ReadinessGate | null = null;
let list: unknown = calList([]);
let applyAnswer: (args: Record<string, unknown>) => unknown = () => { throw new Error("calibration_apply is not expected"); };
let getAnswer: (args: Record<string, unknown>) => unknown = () => ({ found: false, message: "none", record: null, history: [] });

let host: HTMLDivElement, root: Root;
const render = (over: Partial<WorkspaceProps> = {}) => act(async () => root.render(<Workspace {...props(over)} />));
const advance = (ms: number) => act(async () => { await vi.advanceTimersByTimeAsync(ms); });
const calls = () => vi.mocked(invoke).mock.calls.map(([cmd, args]) => ({ cmd, args }));
const applies = () => calls().filter((c) => c.cmd === "calibration_apply");
const $ = (id: string) => host.querySelector<HTMLElement>(`[data-testid="${id}"]`);
const card = () => $("workspace-calibration")!;
const press = async (el: HTMLElement | null) => {
  if (!el) throw new Error("no such control");
  await act(async () => el.click());
  await advance(0);
};

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  vi.useFakeTimers();
  vi.setSystemTime(Date.parse("2026-10-21T10:00:00Z"));
  vi.resetAllMocks();
  calGate = null;
  list = calList([]);
  applyAnswer = () => { throw new Error("calibration_apply is not expected"); };
  getAnswer = () => ({ found: false, message: "none", record: null, history: [] });
  vi.mocked(invoke).mockImplementation(async (cmd: string, args?: unknown) => {
    const a = (args ?? {}) as Record<string, unknown>;
    if (cmd === "fetch_experiment_readiness")
      return { transport_version: JSON_TRANSPORT.version, valid: true, ready: false, generation: "3", gates: calGate ? [calGate, NO_PATH] : [NO_PATH] };
    if (cmd === "calibration_records_list") return list;
    if (cmd === "calibration_record_get") return getAnswer(a);
    if (cmd === "calibration_apply") return applyAnswer(a);
    throw new Error(`unexpected command ${cmd}`);
  });
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => {
  await act(async () => root.unmount());
  host.remove();
  vi.useRealTimers();
});

// After a restart: cal-a is still the applied record, but the method's factor (0.5) stands, so it is
// not in effect; a required method's gate fails and its Fix says to re-apply it (PR #771 ruling).
const APPLIED = calRecord("cal-a", "applied", { factor: 0.52, status_at_utc: "2026-10-21T07:00:00Z" });
const PROPOSED = calRecord("cal-p", "proposed", { factor: 0.53, created_at_utc: "2026-10-21T09:00:00Z" });
const OTHER = calRecord("cal-o", "proposed", { factor: 0.61, identity: { ...SETUP, roi: "0,8,512,96" } });
const INSUFFICIENT = calRecord("cal-i", "insufficient");
const SUPERSEDED = calRecord("cal-s", "superseded", { superseded_by: "cal-a" });
const RESTART_GATE: ReadinessGate = { id: "calibration.record", status: G.Fail,
  reason: "the applied record's factor 0.52 is not the current pixel-to-micron factor 0.5 (it changed after the record was applied)",
  remediation: "re-apply record cal-a (calibration_apply), or run the calibration method" };

describe("what the card shows", () => {
  it("the gate and its Fix, the record in effect with id, factor, age, method and identity, and the plain list", async () => {
    calGate = { id: "calibration.record", status: G.Pass, reason: "", remediation: "" };
    list = calList([APPLIED, SUPERSEDED], { in_effect_record_id: "cal-a", current_factor: 0.52 });
    await render();
    const facts = $("calibration-facts")!;
    expect(facts.textContent).toContain("Calibration gate");
    expect(facts.textContent).toContain("Pass");
    expect(facts.textContent).toContain("cal-a · 0.52 µm/px");
    const effect = $("calibration-in-effect")!;
    expect(effect.textContent).toContain("cal-a");
    expect(effect.textContent).toContain("0.52 µm/px");
    expect(effect.textContent).toContain("3.0 h ago · 2026-10-21T07:00:00Z"); // applied 07:00, now 10:00
    expect(effect.textContent).toContain("Beads 7 µm · revision 3");
    expect(effect.textContent).toContain("camera mock:Camera 01 · ROI full · chip not tracked · objective not tracked");
    const rows = [...$("calibration-list")!.querySelectorAll("tbody tr")].map((r) => r.textContent);
    expect(rows[0]).toContain("Applied · in effect");
    expect(rows[1]).toContain("Superseded");
    expect($("calibration-not-in-effect")).toBeNull();
  });

  it("proposed records, applicable or not; an insufficient record with its failed gates and never an Apply", async () => {
    calGate = { id: "calibration.record", status: G.Warn, reason: "no calibration record is applied",
      remediation: "apply proposed record cal-p (calibration_apply), or run the calibration method" };
    list = calList([PROPOSED, OTHER, INSUFFICIENT]);
    await render();
    expect($("calibration-fix")!.textContent).toBe("Fix: apply proposed record cal-p (calibration_apply), or run the calibration method");
    const p = $("calibration-proposed-cal-p")!;
    expect(p.textContent).toContain("0.5 → 0.53 µm/px · current → proposed");
    expect(p.textContent).toContain("Yes · on this setup");
    expect(p.querySelector<HTMLButtonElement>("button")!.disabled).toBe(false);
    const o = $("calibration-proposed-cal-o")!;
    expect(o.textContent).toContain("No · derived under another setup (ROI 0,8,512,96 → full)");
    expect(o.querySelector<HTMLButtonElement>("button")!.disabled).toBe(true);
    const i = $("calibration-insufficient-cal-i")!;
    expect(i.textContent).toContain("Valid objects: 120, at least 200");
    expect(i.textContent).toContain("Diameter CV: 14.2 %, at most 10 %");
    expect(i.querySelector("button")).toBeNull();
    expect($("calibration-insufficient")!.textContent).toContain("never applicable");
    expect(card().textContent).toContain("None in effect");
  });

  it("running a calibration points to the Plan's Start and says what is derived; the purpose gap is said, not faked", async () => {
    await render();
    const run = $("calibration-run")!;
    expect(run.textContent).toContain("Experiment · Plan with Start run, like any run");
    expect(run.textContent).toContain("proposed pixel-to-micron (the reference's nominal diameter ÷ the median)");
    expect(run.textContent).toContain("at least 200 valid objects");
    expect($("calibration-purpose-gap")!.textContent).toContain("cannot tell yet whether the applied method is a calibration method");
  });

  it("an unread list and an unread gate are unknown, never None", async () => {
    vi.mocked(invoke).mockImplementation(async (cmd: string) => {
      if (cmd === "fetch_experiment_readiness") throw new Error("TRANSPORT_LOST");
      if (cmd === "calibration_records_list") throw new Error("TRANSPORT_LOST");
      throw new Error(`unexpected ${cmd}`);
    });
    await render();
    expect($("calibration-facts")!.textContent).toContain("Unknown · Error: TRANSPORT_LOST");
    expect(card().textContent).not.toContain("None in effect");
    expect(card().querySelector(".mib-card__status")?.textContent ?? card().textContent).toContain("Unknown");
  });
});

describe("only Apply / Re-apply acts (IPC calls counted)", () => {
  it("showing, polling, selecting a record and leaving never send calibration_apply", async () => {
    calGate = RESTART_GATE;
    list = calList([PROPOSED, APPLIED, INSUFFICIENT]);
    getAnswer = (a) => ({ found: true, message: "", record: PROPOSED, history: [{ ...PROPOSED }], ...(a.recordId === "cal-p" ? {} : { found: false }) });
    await render();
    await advance(PREPARE_READINESS_POLL_MS * 10);
    const row = [...$("calibration-list")!.querySelectorAll("tbody tr")].find((r) => r.textContent!.includes("cal-p"))!;
    await press(row as HTMLElement);
    expect($("calibration-history")!.textContent).toContain("v1 · proposed");
    await render({ stage: 2 }); // the Plan
    await advance(PREPARE_READINESS_POLL_MS * 5);
    await render({ stage: 1 });
    await advance(PREPARE_READINESS_POLL_MS * 5);
    await render({ stage: 0 });
    await advance(PREPARE_READINESS_POLL_MS * 5);
    expect(applies()).toEqual([]);
    const kinds = new Set(calls().map((c) => c.cmd));
    expect([...kinds].sort()).toEqual(["calibration_record_get", "calibration_records_list", "fetch_experiment_readiness"]);
    expect(calls().filter((c) => c.cmd === "calibration_record_get")).toEqual([{ cmd: "calibration_record_get", args: { recordId: "cal-p" } }]);
  });

  it("after a restart the gate's Fix offers Re-apply directly: one calibration_apply with the factor the page read; previous → new from the reply", async () => {
    calGate = RESTART_GATE;
    list = calList([PROPOSED, APPLIED]);
    applyAnswer = () => applyReply("cal-a", O.Applied, { previous_factor: 0.5, new_factor: 0.52 });
    await render();
    expect($("calibration-not-in-effect")!.textContent).toContain("Its factor 0.52 µm/px is not the current factor 0.5 µm/px");
    const fix = $("calibration-facts")!.querySelector<HTMLButtonElement>('[data-testid="calibration-reapply-cal-a"]')!;
    expect(fix.textContent).toBe("Re-apply cal-a");
    const listReads = calls().filter((c) => c.cmd === "calibration_records_list").length;
    list = calList([PROPOSED, { ...APPLIED, version: 2, status_at_utc: "2026-10-21T10:00:00Z" }], { in_effect_record_id: "cal-a", current_factor: 0.52 });
    calGate = { id: "calibration.record", status: G.Pass, reason: "", remediation: "" };
    await press(fix);
    expect(applies()).toEqual([{ cmd: "calibration_apply", args: { recordId: "cal-a", expectedCurrentFactor: 0.5 } }]);
    const outcome = $("calibration-outcome")!;
    expect(outcome.textContent).toContain("Applied · cal-a");
    expect(outcome.textContent).toContain("Pixel-to-micron 0.5 → 0.52 µm/px");
    // Re-read at once (not only at the next poll): the record is in effect now.
    expect(calls().filter((c) => c.cmd === "calibration_records_list").length).toBeGreaterThan(listReads);
    expect($("calibration-in-effect")!.textContent).toContain("cal-a");
    expect($("calibration-not-in-effect")).toBeNull();
    await advance(PREPARE_READINESS_POLL_MS * 5);
    expect(applies()).toHaveLength(1); // nothing more on its own
  });

  it("Apply of a proposal sends its id with the factor read; the reply's numbers are shown, not the list's", async () => {
    list = calList([PROPOSED]);
    applyAnswer = () => applyReply("cal-p", O.Applied, { previous_factor: 0.5000001, new_factor: 0.5299999, superseded_record_id: "cal-a" });
    await render();
    await press($("calibration-proposed-cal-p")!.querySelector("button"));
    expect(applies()).toEqual([{ cmd: "calibration_apply", args: { recordId: "cal-p", expectedCurrentFactor: 0.5 } }]);
    expect($("calibration-outcome")!.textContent).toContain("Pixel-to-micron 0.5 → 0.53 µm/px. Record cal-a is superseded.");
  });

  it.each([
    ["a stale factor", O.StaleFactor, { previous_factor: 0.51, message: "the current pixel-to-micron factor is 0.51, not the expected 0.5; nothing was applied" }, "Not applied · the factor changed since this page read it"],
    ["an active run", O.RunActive, { message: "the experiment is Active: calibration_apply is refused unless it is idle; nothing was applied" }, "Not applied · a run is active"],
    ["an insufficient record", O.Insufficient, { message: "record cal-p is insufficient" }, "Not applied · the record is insufficient"],
    ["another setup", O.IdentityMismatch, { message: "record cal-p was derived under another setup (ROI full -> 0,8,512,96)" }, "Not applied · the record is from another setup"],
    ["an unknown identity", O.IdentityMismatch, { message: "the camera or ROI identity is unknown now; connect the camera and let the ROI apply first" }, "Not applied · the setup's identity is unknown"],
  ])("a refusal (%s) is said in plain words; nothing changes on screen but the re-read", async (_what, outcome, over, title) => {
    list = calList([PROPOSED]);
    applyAnswer = () => applyReply("cal-p", outcome, over);
    await render();
    const before = $("calibration-proposed-cal-p")!.textContent;
    await press($("calibration-proposed-cal-p")!.querySelector("button"));
    expect(applies()).toHaveLength(1);
    expect($("calibration-outcome")!.textContent).toContain(title);
    expect($("calibration-outcome")!.textContent).toContain("Nothing was applied");
    expect($("calibration-proposed-cal-p")!.textContent).toBe(before); // the list re-read says the same
    expect($("calibration-in-effect")).toBeNull();
  });

  it("a viewer's Apply is disabled with the shared control block, and nothing is sent", async () => {
    calGate = RESTART_GATE;
    list = calList([PROPOSED, APPLIED]);
    await render({ controlHeld: false });
    const control = $("calibration-control")!;
    expect(control.textContent).toContain("Another tab or client holds control");
    expect(control.textContent).toContain("Apply is unavailable here, and nothing is sent.");
    for (const b of card().querySelectorAll<HTMLButtonElement>('button[data-testid^="calibration-"]')) {
      expect(b.disabled).toBe(true);
      expect(b.title).toBe("another tab or client holds control");
      await press(b);
    }
    expect(applies()).toEqual([]);
  });

  it("the server's VIEWER_ONLY refusal (control lost meanwhile) is said in plain words", async () => {
    list = calList([PROPOSED]);
    applyAnswer = () => { throw new Error("VIEWER_ONLY: another client controls this instrument"); };
    await render();
    await press($("calibration-proposed-cal-p")!.querySelector("button"));
    expect($("calibration-outcome")!.textContent).toContain("Not applied · another tab or client holds control");
  });

  it("during a run nothing is read or sent", async () => {
    list = calList([PROPOSED]);
    await render({ experimentActive: true });
    await advance(PREPARE_READINESS_POLL_MS * 3);
    expect(calls()).toEqual([]);
    expect($("calibration-facts")!.textContent).toContain("Unknown · not read while a run is active");
  });

  it("a list read that started before the apply answered is dropped: the factor shown is never older than the apply", async () => {
    list = calList([PROPOSED]);
    await render();
    // Hold every list read from now on; the test answers each one.
    const held: ((v: unknown) => void)[] = [];
    vi.mocked(invoke).mockImplementation(async (cmd: string) => {
      if (cmd === "fetch_experiment_readiness") return { transport_version: JSON_TRANSPORT.version, valid: true, ready: false, generation: "3", gates: [NO_PATH] };
      if (cmd === "calibration_records_list") return new Promise((resolve) => held.push(resolve));
      if (cmd === "calibration_apply") return applyReply("cal-p", O.Applied, { previous_factor: 0.5, new_factor: 0.53 });
      throw new Error(`unexpected ${cmd}`);
    });
    await advance(PREPARE_READINESS_POLL_MS); // a poll starts and is held
    expect(held).toHaveLength(1);
    await press($("calibration-proposed-cal-p")!.querySelector("button"));
    expect(applies()).toHaveLength(1);
    // The poll from before the apply answers late, with what the backend said then.
    await act(async () => held[0](calList([PROPOSED], { current_factor: 0.4999 })));
    await advance(0);
    expect($("calibration-facts")!.textContent).not.toContain("0.4999"); // dropped
    // It is read again at once, and that read is shown.
    expect(held).toHaveLength(2);
    await act(async () => held[1](calList([{ ...PROPOSED, status: "applied", applicable: false, apply: { previous_factor: 0.5, new_factor: 0.53 } }],
      { applied_record_id: "cal-p", in_effect_record_id: "cal-p", current_factor: 0.53 })));
    await advance(0);
    expect($("calibration-facts")!.textContent).toContain("cal-p · 0.53 µm/px");
    expect($("calibration-facts")!.textContent).toContain("Current factor0.53 µm/px");
  });

  it("an older record_get reply never replaces a newer one (Codex round 1)", async () => {
    list = calList([PROPOSED]);
    const held: ((v: unknown) => void)[] = [];
    getAnswer = () => new Promise((resolve) => held.push(resolve));
    applyAnswer = () => applyReply("cal-p", O.Applied, { previous_factor: 0.5, new_factor: 0.53 });
    await render();
    const row = [...$("calibration-list")!.querySelectorAll("tbody tr")].find((r) => r.textContent!.includes("cal-p"))!;
    await press(row as HTMLElement); // get #1, held
    await press($("calibration-proposed-cal-p")!.querySelector("button")); // the apply re-reads the versions: get #2
    expect(held).toHaveLength(2);
    await act(async () => held[1]({ found: true, message: "", record: PROPOSED, history: [PROPOSED, { ...PROPOSED, version: 2, status: "applied" }] }));
    await act(async () => held[0]({ found: true, message: "", record: PROPOSED, history: [PROPOSED] })); // the older reply, late
    await advance(0);
    expect($("calibration-history")!.textContent).toContain("v2 · applied");
  });

  it("a list read from before a run is dropped when the card is read again after it (Codex round 1)", async () => {
    list = calList([PROPOSED]);
    await render();
    const held: ((v: unknown) => void)[] = [];
    vi.mocked(invoke).mockImplementation(async (cmd: string) => {
      if (cmd === "fetch_experiment_readiness") return { transport_version: JSON_TRANSPORT.version, valid: true, ready: false, generation: "3", gates: [NO_PATH] };
      if (cmd === "calibration_records_list") return new Promise((resolve) => held.push(resolve));
      throw new Error(`unexpected ${cmd}`);
    });
    await advance(PREPARE_READINESS_POLL_MS); // a poll starts and is held
    await render({ experimentActive: true }); // a run starts: no reads
    await render({ experimentActive: false }); // it ends: read again
    await act(async () => held[0](calList([PROPOSED], { current_factor: 0.4444 }))); // the read from before the run answers late
    await advance(0);
    expect($("calibration-facts")!.textContent).not.toContain("0.4444");
    await act(async () => held[held.length - 1](calList([PROPOSED], { current_factor: 0.5 })));
    await advance(0);
    expect($("calibration-facts")!.textContent).toContain("Current factor0.5 µm/px");
  });

  it("an unreadable store is unknown, never None in effect (Codex round 1)", async () => {
    list = calList([], { store: { revision: 0, max_versions: 2048, error: "/data/instrument_calibration.json is unreadable (bad json)" } });
    await render();
    expect($("calibration-facts")!.textContent).toContain("Unknown · the calibration store cannot be read");
    expect(card().textContent).not.toContain("None in effect");
    expect($("calibration-store-error")!.textContent).toContain("is unreadable (bad json)");
    expect($("calibration-facts")!.textContent).toContain("Current factor0.5 µm/px");
  });
});
