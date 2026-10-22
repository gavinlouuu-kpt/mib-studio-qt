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
import type { MethodCalibration } from "../calibrationRecords";
import type { PreflightInput } from "../preflight";
import { applyReply, calList, calRecord, SETUP } from "./calibrationFixtures";
import { PREPARE_READINESS_POLL_MS } from "./prepareModel";
import { RECONCILE_MS } from "./useCalibration";
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
  start: async () => null, newPlan: () => {}, reference: null, setReference: () => {},
  queue: { state: newQueue(), pending: null, controlHeld: true, status: null, startCurrent: async () => {}, stopCurrent: async () => {},
    confirmClosed: () => {}, skip: () => {}, end: () => {}, resume: () => {}, repeat: () => {} },
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
/** The readiness report's method_calibration (ABI 43); undefined = a backend before ABI 43 (absent). */
let methodCal: MethodCalibration | undefined = undefined;
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
  methodCal = undefined;
  list = calList([]);
  applyAnswer = () => { throw new Error("calibration_apply is not expected"); };
  getAnswer = () => ({ found: false, message: "none", record: null, history: [] });
  vi.mocked(invoke).mockImplementation(async (cmd: string, args?: unknown) => {
    const a = (args ?? {}) as Record<string, unknown>;
    if (cmd === "fetch_experiment_readiness")
      return { transport_version: JSON_TRANSPORT.version, valid: true, ready: false, generation: "3", gates: calGate ? [calGate, NO_PATH] : [NO_PATH],
        ...(methodCal === undefined ? {} : { method_calibration: methodCal }) };
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
    // A backend before ABI 43 reports no method: nothing about it is said or guessed.
    expect($("calibration-method")).toBeNull();
    expect($("calibration-policy")).toBeNull();
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

// ABI 43 (#776; #780 "For the A4b card"): the card reads the report's method_calibration from the
// readiness read that already happens; purpose and policy are never parsed from the gate's text.
const UNDECLARED: MethodCalibration = { declared: false, purpose: "measurement", calibration_policy: "none", calibration_max_age_hours: 24, calibration_reference: null, error: null };
const BEADS: MethodCalibration = { declared: true, purpose: "calibration", calibration_policy: "none", calibration_max_age_hours: 24, error: null,
  calibration_reference: { nominal_diameter_um: 7, diameter_tolerance_um: 0.5, nominal_e_kpa: 3, max_cv_percent: 10 } };
const NOT_REQUIRED: ReadinessGate = { id: "calibration.record", status: G.NotRequired, reason: "a calibration method records the reference", remediation: "" };

describe("the applied method's calibration declarations (ABI 43)", () => {
  it("a calibration method: says closing its run derives a record, with its reference; no purpose-gap note", async () => {
    calGate = NOT_REQUIRED;
    methodCal = BEADS;
    await render();
    expect($("calibration-method")!.textContent).toBe("This method is a calibration method: closing its run derives a record");
    expect($("calibration-reference")!.textContent).toBe("7 ± 0.5 µm diameter · CV at most 10 % · nominal E 3 kPa");
    expect($("calibration-policy")!.textContent).toContain("none · max age 24 h");
    expect($("calibration-policy")!.textContent).toContain("The gate requires no record for a calibration method itself.");
    expect($("calibration-run")!.textContent).toContain("The applied method is a calibration method: record the beads from Experiment · Plan with Start run");
    expect($("calibration-purpose-gap")).toBeNull();
  });

  it("a calibration method without a nominal E shows none", async () => {
    methodCal = { ...BEADS, calibration_reference: { ...BEADS.calibration_reference!, nominal_e_kpa: null, max_cv_percent: 8 } };
    await render();
    expect($("calibration-reference")!.textContent).toBe("7 ± 0.5 µm diameter · CV at most 8 %");
  });

  it("policy and max age come from the report, not the gate's text", async () => {
    // The gate's words name a calibration method; the report says a measurement method with a policy.
    calGate = { ...NOT_REQUIRED, status: G.Fail, reason: "no calibration record is applied", remediation: "run the calibration method" };
    methodCal = { ...UNDECLARED, declared: true, calibration_policy: "required", calibration_max_age_hours: 12 };
    await render();
    expect($("calibration-method")!.textContent).toBe("Measurement method");
    expect($("calibration-policy")!.textContent).toContain("required · max age 12 h");
    expect($("calibration-policy")!.textContent).toContain("Start needs an applied record in effect; one older than 12 h warns.");
    expect($("calibration-reference")).toBeNull();
    expect($("calibration-purpose-gap")).toBeNull();
  });

  it("declared false is said plainly: a measurement method with policy none, even under gate words about calibration", async () => {
    calGate = NOT_REQUIRED;
    methodCal = UNDECLARED;
    await render();
    expect($("calibration-method")!.textContent).toBe("Measurement method · policy none: it declares no calibration requirements");
    expect($("calibration-policy")!.textContent).toContain("none · max age 24 h");
    expect($("calibration-run")!.textContent).toContain("Apply the calibration method");
    expect($("calibration-purpose-gap")).toBeNull();
  });

  it("unreadable declarations: the error beside the failing gate; no defaults shown as the method's", async () => {
    calGate = { id: "calibration.record", status: G.Fail, reason: "the applied method's calibration declarations are malformed", remediation: "fix the method's calibration members" };
    methodCal = { ...UNDECLARED, declared: true, error: "calibration_policy must be one of none, warn, required" };
    await render();
    const gateRow = $("calibration-fix")!.parentElement!;
    expect(gateRow.querySelector('[data-testid="calibration-method-error"]')!.textContent)
      .toBe("The applied method's calibration declarations cannot be read: calibration_policy must be one of none, warn, required");
    expect($("calibration-method")!.textContent).toBe("Unknown · its calibration declarations cannot be read");
    expect($("calibration-policy")).toBeNull();
    expect($("calibration-reference")).toBeNull();
    expect($("calibration-purpose-gap")).toBeNull();
  });

  it("no extra IPC: the report rides on the readiness read that already happens", async () => {
    const run = async () => {
      vi.mocked(invoke).mockClear();
      await render();
      await advance(PREPARE_READINESS_POLL_MS * 4);
      const counts: Record<string, number> = {};
      for (const c of calls()) counts[c.cmd] = (counts[c.cmd] ?? 0) + 1;
      await act(async () => root.unmount());
      root = createRoot(host);
      return counts;
    };
    const older = await run();
    methodCal = BEADS;
    const abi43 = await run();
    expect(abi43).toEqual(older);
    expect(Object.keys(abi43).sort()).toEqual(["calibration_records_list", "fetch_experiment_readiness"]);
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

  it("Apply stays locked until the list read after its answer settles: a second press sends nothing (Codex round 2)", async () => {
    list = calList([PROPOSED]);
    let n = 0;
    applyAnswer = () => (++n === 1 ? applyReply("cal-p", O.Applied, { previous_factor: 0.5, new_factor: 0.53 })
      : applyReply("cal-p", O.NotApplicable, { message: "record cal-p is already applied and in effect" }));
    await render();
    const held: ((v: unknown) => void)[] = [];
    vi.mocked(invoke).mockImplementation(async (cmd: string, args?: unknown) => {
      if (cmd === "fetch_experiment_readiness") return { transport_version: JSON_TRANSPORT.version, valid: true, ready: false, generation: "3", gates: [NO_PATH] };
      if (cmd === "calibration_records_list") return new Promise((resolve) => held.push(resolve));
      if (cmd === "calibration_apply") return applyAnswer((args ?? {}) as Record<string, unknown>);
      throw new Error(`unexpected ${cmd}`);
    });
    const button = () => $("calibration-proposed-cal-p")!.querySelector<HTMLButtonElement>("button")!;
    await press(button());
    expect($("calibration-outcome")!.textContent).toContain("Applied · cal-p");
    expect(button().disabled).toBe(true); // the list from before the apply is still shown
    await press(button());
    expect(applies()).toHaveLength(1);
    await act(async () => held[held.length - 1](calList([{ ...PROPOSED, status: "applied", applicable: false, apply: { previous_factor: 0.5, new_factor: 0.53 } }],
      { applied_record_id: "cal-p", in_effect_record_id: "cal-p", current_factor: 0.53 })));
    await advance(0);
    expect($("calibration-proposed-cal-p")).toBeNull(); // in effect now, no Apply left
    expect($("calibration-outcome")!.textContent).toContain("Applied · cal-p");
  });

  it("a list read that never answers unlocks Apply after RECONCILE_MS", async () => {
    list = calList([PROPOSED]);
    applyAnswer = () => applyReply("cal-p", O.StaleFactor, { previous_factor: 0.51 });
    await render();
    vi.mocked(invoke).mockImplementation(async (cmd: string, args?: unknown) => {
      if (cmd === "fetch_experiment_readiness") return { transport_version: JSON_TRANSPORT.version, valid: true, ready: false, generation: "3", gates: [NO_PATH] };
      if (cmd === "calibration_records_list") return new Promise(() => {});
      if (cmd === "calibration_apply") return applyAnswer((args ?? {}) as Record<string, unknown>);
      throw new Error(`unexpected ${cmd}`);
    });
    const button = () => $("calibration-proposed-cal-p")!.querySelector<HTMLButtonElement>("button")!;
    await press(button());
    expect(button().disabled).toBe(true);
    await advance(RECONCILE_MS);
    expect(button().disabled).toBe(false);
  });

  it("the gates read after an apply is never replaced by a poll from before it (Codex round 2)", async () => {
    list = calList([PROPOSED]);
    calGate = { id: "calibration.record", status: G.Warn, reason: "no calibration record is applied", remediation: "apply proposed record cal-p (calibration_apply), or run the calibration method" };
    await render();
    const heldGates: ((v: unknown) => void)[] = [];
    let applied = false;
    const gates = (g: ReadinessGate) => ({ transport_version: JSON_TRANSPORT.version, valid: true, ready: false, generation: "4", gates: [g, NO_PATH] });
    vi.mocked(invoke).mockImplementation(async (cmd: string) => {
      if (cmd === "fetch_experiment_readiness") return new Promise((resolve) => heldGates.push(resolve));
      if (cmd === "calibration_records_list") return applied ? calList([{ ...PROPOSED, status: "applied", applicable: false, apply: { previous_factor: 0.5, new_factor: 0.53 } }],
        { applied_record_id: "cal-p", in_effect_record_id: "cal-p", current_factor: 0.53 }) : calList([PROPOSED]);
      if (cmd === "calibration_apply") { applied = true; return applyReply("cal-p", O.Applied, { previous_factor: 0.5, new_factor: 0.53 }); }
      throw new Error(`unexpected ${cmd}`);
    });
    await advance(PREPARE_READINESS_POLL_MS); // a gates poll starts and is held
    expect(heldGates).toHaveLength(1);
    await press($("calibration-proposed-cal-p")!.querySelector("button"));
    // The poll from before the apply answers late, with what the backend said then.
    await act(async () => heldGates[0](gates({ ...calGate!, reason: "stale: from before the apply" })));
    await advance(0);
    expect(heldGates).toHaveLength(2); // read once more
    expect(host.querySelector('[data-row="calibration"]')!.textContent).not.toContain("stale: from before the apply"); // dropped, in the row's chip too
    await act(async () => heldGates[1](gates({ id: "calibration.record", status: G.Pass, reason: "", remediation: "" })));
    await advance(0);
    // Prepare's Calibration row (O1b) carries the gate's chip: Pass, with the record in effect.
    expect(host.querySelector('[data-row="calibration"] > summary .mib-chip')!.textContent).toBe("Pass · in effect");
  });
});
