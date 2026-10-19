// @vitest-environment jsdom
// The guided sample queue in Experiment · Plan (#716 A5), with the real plan state (usePlan) over a
// mocked Start flow, stop and experiment status: each step is one button and one command at most;
// nothing advances, stops or starts on its own; failures stop the flow with the operator's choice;
// identities follow #736 (a started or unknown run index is never sent again); a lost controlling
// client pauses the flow until the operator resumes it; the PZ7035 Run switch is a button.
import { act, StrictMode, useState } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { invoke } from "@tauri-apps/api/core";
import { EXPERIMENT_STATES as S, JSON_TRANSPORT, READINESS_GATE_STATUSES as G, RUN_COMPLETION_STATES as C } from "../bridgeContract";
import type { ExperimentStatus } from "../eventAdapter";
import type { PreflightInput } from "../preflight";
import type { AlignFacts } from "./alignControls";
import type { ExperimentSubView } from "./ExperimentView";
import { planRunMode, type PlanRunMode } from "./planModel";
import { usePlan, type PlanStartResult, type StopReply } from "./usePlan";
import { Workspace, type WorkspaceProps } from "./Workspace";

vi.mock("@tauri-apps/api/core", () => ({ invoke: vi.fn() }));

const OFF = { valid: true, connected: false, identity: "" };
const READY_INPUT: PreflightInput = {
  backendReady: true, cameraConfigured: true, cameraRunning: true, cameraIdentity: "Mock camera", cameraExpected: "",
  coreKnown: true, coreValid: true, corePinSatisfied: true, coreVersion: "2.1.0", requiredCoreVersion: "2.1.0",
  autofocus: OFF, samplePump: OFF, sheathPump: OFF, trigger: { valid: true, cameraAttached: false },
  storageKnown: false, storageWritable: false, storageFreeOk: false, storagePath: "",
};
const FACTS: AlignFacts = {
  pz7035: false, instrumentModes: false, hostProcessing: true, geometry: null, instrumentMode: null, runFrozen: false, modeError: "",
  appliedRunWindow: null, placedRunWindow: null, processingRoi: null, runFps: null, experimentActive: false, running: false,
};
const status = (over: Partial<ExperimentStatus> = {}): ExperimentStatus => ({
  valid: true, state: S.Idle, start_time_ns: "0", end_time_ns: "0", valid_buffered: "0", invalid_buffered: "0", valid_saved: "0", invalid_saved: "0",
  dropped_valid: "0", dropped_invalid: "0", flushing: false, cancelled: false, output_path: "", message: "", start_generation: "0", readiness_generation: "1",
  capture_generation: "1", persistence_admitted: "0", persistence_committed: "0", persistence_failed: "0", terminal: false, finalization_ok: false,
  completion: C.Complete, completion_reason: "", fault_revision: "0", fault_code: "", fault_message: "", ...over,
});

let host: HTMLDivElement, root: Root;
let runStart = vi.fn(async (_json: string): Promise<PlanStartResult> => ({ kind: "started", path: "/data/run.h5", notice: "" }));
let stop = vi.fn(async (): Promise<StopReply> => ({ ok: true, message: "stop accepted" }));
const onSwitchToRun = vi.fn(async () => {});
/** The test's handles on the harness's backend facts. */
let setStatus: (s: ExperimentStatus | null) => void = () => {};
let setControl: (held: boolean) => void = () => {};

function Harness({ runMode = null }: { runMode?: PlanRunMode | null }) {
  const [st, setSt] = useState<ExperimentStatus | null>(status());
  const [held, setHeld] = useState(true);
  setStatus = setSt; setControl = setHeld;
  const plan = usePlan(runStart, { status: st, stop, controlHeld: held });
  const [view, setView] = useState<ExperimentSubView>("Plan");
  const props: WorkspaceProps = {
    brand: "MIB Studio", source: "Mock camera", method: "", stage: 2, onStage: () => {}, onExit: () => {}, serviceMode: false,
    preflightInput: READY_INPUT, zStage: null, signature: "sig", confirmedFor: "", onConfirm: () => {}, confirmLockedReason: "",
    experimentActive: false, onRefreshChecks: () => {}, onOpenSetup: () => {},
    align: { facts: FACTS, subscribeFrames: () => () => {}, onSwitchMode: async () => {}, writeRoi: vi.fn(), lockedReason: "" },
    experiment: { plan, lockedReason: "", view, onView: setView, runMode, onSwitchToRun },
  };
  return <Workspace {...props} />;
}

const render = (props: { runMode?: PlanRunMode | null } = {}) => act(async () => root.render(<StrictMode><Harness {...props} /></StrictMode>));
const advance = (ms: number) => act(async () => { await vi.advanceTimersByTimeAsync(ms); });
const $ = (id: string) => host.querySelector<HTMLElement>(`[data-testid=${id}]`);
const button = (text: string) => [...host.querySelectorAll("button")].find((b) => b.textContent === text) as HTMLButtonElement | undefined;
const press = async (el: HTMLElement | null | undefined, what: string) => {
  if (!el) throw new Error(`no ${what}`);
  await act(async () => el.click());
};
async function type(testId: string, value: string) {
  const el = host.querySelector<HTMLInputElement>(`[data-testid=${testId}]`);
  if (!el) throw new Error(`no field ${testId}`);
  await act(async () => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(el, value);
    el.dispatchEvent(new Event("input", { bubbles: true }));
  });
}
const report = async (s: Partial<ExperimentStatus>) => act(async () => setStatus(status(s)));
const experimentId = () => $("plan-experiment-id")!.textContent!;
const selectRow = async (ref: string) => {
  const row = [...host.querySelectorAll<HTMLTableRowElement>("[data-testid=plan-sheet-table] tbody tr")].find((tr) => tr.children[1]?.textContent === ref);
  await press(row, `row ${ref}`);
};
const stateOf = (ref: string) => $(`queue-state-${ref}`)!.textContent;
const stepStates = () => [...host.querySelectorAll("[data-testid=queue-stepper] li")].map((li) => li.getAttribute("data-state"));
/** Everything that acts: the Start flow and the stop. */
const acted = () => runStart.mock.calls.length + stop.mock.calls.length;

/** A queue of S1 (Vehicle) and S2 (Cytochalasin D). */
async function twoSamples() {
  await render();
  await press(host.querySelectorAll<HTMLInputElement>(".mib-option input")[1], "Sample queue");
  await type("plan-condition", "Vehicle");
  await press($("queue-add"), "+ Sample");
  await type("plan-condition", "Cytochalasin D");
}

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  vi.useFakeTimers();
  vi.resetAllMocks();
  runStart = vi.fn(async (_json: string): Promise<PlanStartResult> => ({ kind: "started", path: "/data/run.h5", notice: "" }));
  stop = vi.fn(async (): Promise<StopReply> => ({ ok: true, message: "stop accepted" }));
  const saved = new Map<string, string>();
  vi.stubGlobal("localStorage", { getItem: (k: string) => saved.get(k) ?? null, setItem: (k: string, v: string) => void saved.set(k, v), removeItem: (k: string) => void saved.delete(k) });
  vi.mocked(invoke).mockImplementation(async (cmd: string) => {
    if (cmd === "fetch_experiment_readiness") return { transport_version: JSON_TRANSPORT.version, valid: true, ready: false, generation: "1",
      gates: [{ id: "storage.output", status: G.Unavailable, reason: "no output path chosen yet", remediation: "choose an HDF5 destination" }] };
    throw new Error(`unexpected command ${cmd}`);
  });
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => {
  await act(async () => root.unmount());
  host.remove();
  vi.useRealTimers();
  vi.unstubAllGlobals();
});

describe("the sample format", () => {
  it("offers a single sample and the sample queue; with several samples the single format says why it is unavailable", async () => {
    await render();
    const options = () => [...host.querySelectorAll<HTMLLabelElement>(".mib-option")];
    expect(options().map((o) => o.querySelector(".mib-option__label")!.textContent)).toEqual(["Single sample", "Sample queue"]);
    expect(options()[1].querySelector("input")!.disabled).toBe(false);
    await press(options()[1].querySelector("input"), "Sample queue");
    expect($("queue-card")).not.toBeNull();
    await press($("queue-add"), "+ Sample");
    expect(options()[0].querySelector("input")!.disabled).toBe(true);
    expect(options()[0].querySelector(".mib-option__reason")!.textContent).toBe("The sheet has 2 samples · remove all but one first");
    expect(acted()).toBe(0);
  });
});

describe("the sheet", () => {
  it("holds one row per sample with its own ref under one experiment id; editing, adding, moving and removing send nothing", async () => {
    await twoSamples();
    const id = experimentId();
    await press($("queue-add"), "+ Sample");
    await type("plan-sample", "beads");
    const refs = () => [...host.querySelectorAll("[data-testid=plan-sheet-table] tbody tr")].map((tr) => tr.children[1].textContent);
    expect(refs()).toEqual(["S1", "S2", "S3"]);
    // Planned run indexes 1..N, in order.
    expect([...host.querySelectorAll("[data-testid=plan-sheet-table] tbody tr")].map((tr) => tr.children[0].textContent)).toEqual(["1", "2", "3"]);
    await press(host.querySelector('[aria-label="Move S3 up"]') as HTMLElement, "Move S3 up");
    expect(refs()).toEqual(["S1", "S3", "S2"]);
    await press(host.querySelector('[aria-label="Remove S2"]') as HTMLElement, "Remove S2");
    expect(refs()).toEqual(["S1", "S3"]);
    await press($("queue-add"), "+ Sample");
    expect(refs()).toEqual(["S1", "S3", "S4"]); // a removed ref is not given out again
    // One colour per condition, as in the single plan.
    expect([...host.querySelectorAll("[data-testid=plan-sheet-table] .mib-condition")].map((c) => c.textContent)).toEqual(["Vehicle"]);
    expect(experimentId()).toBe(id);
    await advance(5000);
    expect(acted()).toBe(0);
  });
});

describe("the guided flow", () => {
  it("runs two samples with exactly the operator's presses: Start S1, Next sample, Confirm closed, Start S2, Stop last sample, Confirm closed", async () => {
    await twoSamples();
    const id = experimentId();
    expect(stepStates()).toEqual(["current", "pending", "pending", "pending", "pending", "pending"]);
    expect($("queue-step")!.textContent).toContain("Ready for S1 · Vehicle");

    // Start S1: one Start flow, with run 1 and sample_ref S1.
    await press($("queue-start"), "Start S1");
    expect(runStart).toHaveBeenCalledTimes(1);
    expect(runStart.mock.calls[0][0]).toBe(`{"experiment_id":"${id}","run_index":1,"sample_ref":"S1","annotation":{"condition":"Vehicle"}}`);
    expect(stateOf("S1")).toBe("Running");
    expect(($("plan-condition") as HTMLInputElement).disabled).toBe(false); // S2 (selected since it was added) is still editable
    await selectRow("S1");
    expect(($("plan-condition") as HTMLInputElement).disabled).toBe(true); // S1 is locked now
    expect($("queue-locked-note")!.textContent).toContain("S1 is locked: its run started (run 1)");
    expect((host.querySelector('[aria-label="Move S2 up"]') as HTMLButtonElement).disabled).toBe(true); // not above the started S1
    expect(host.querySelector('[aria-label="Move S1 down"]')).toBeNull(); // S1 keeps its place
    expect((host.querySelector('[aria-label="Move S2 down"]') as HTMLButtonElement).disabled).toBe(true);
    // The backend reports the run; time passes: nothing else is sent.
    await report({ state: S.Active, start_generation: "1", output_path: "/data/s1.h5", valid_saved: "40", persistence_admitted: "50", persistence_committed: "40" });
    await advance(10_000);
    expect(acted()).toBe(1);
    expect($("queue-step")!.textContent).toContain("S1 running · run 1 · /data/s1.h5");
    expect($("queue-progress-facts")!.textContent).toContain("Committed / admitted40 / 50");
    expect(stepStates()).toEqual(["done", "done", "current", "pending", "pending", "pending"]);
    expect($("queue-file-S1")!.textContent).toBe("s1.h5");

    // Next sample: one stop; the drain and finalize are followed in the status.
    await press(button("Next sample"), "Next sample");
    expect(stop).toHaveBeenCalledTimes(1);
    await report({ state: S.Stopping, flushing: true, start_generation: "1", output_path: "/data/s1.h5" });
    expect($("queue-step")!.textContent).toContain("Stopping S1 · draining and finalizing");
    expect(stepStates()).toEqual(["done", "done", "done", "current", "pending", "pending"]);
    await report({ state: S.Idle, terminal: true, finalization_ok: true, start_generation: "1", output_path: "/data/s1.h5", persistence_admitted: "50", persistence_committed: "50" });
    expect($("queue-step")!.textContent).toContain("S1 finalized · the backend closed /data/s1.h5");
    expect(stepStates()).toEqual(["done", "done", "done", "done", "done", "current"]);
    await advance(10_000);
    expect(acted()).toBe(2); // the close waits for the operator

    // Confirm closed: sends nothing; the prompt for the next tube; S2 waits for its Start.
    await press(button("Confirm closed"), "Confirm closed");
    expect(acted()).toBe(2);
    expect(stateOf("S1")).toBe("Closed");
    expect($("queue-step")!.textContent).toContain("Swap the tube for S2 · Cytochalasin D");
    expect($("queue-step")!.textContent).toContain("S1 is closed (/data/s1.h5)");
    await advance(10_000);
    expect(acted()).toBe(2);

    // Start S2: run 2, sample_ref S2, the same experiment.
    await press(button("Start S2"), "Start S2");
    expect(runStart).toHaveBeenCalledTimes(2);
    expect(runStart.mock.calls[1][0]).toBe(`{"experiment_id":"${id}","run_index":2,"sample_ref":"S2","annotation":{"condition":"Cytochalasin D"}}`);
    await report({ state: S.Active, start_generation: "2", output_path: "/data/s2.h5" });
    await press(button("Stop last sample"), "Stop last sample");
    expect(stop).toHaveBeenCalledTimes(2);
    await report({ state: S.Idle, terminal: true, finalization_ok: true, start_generation: "2", output_path: "/data/s2.h5" });
    await press(button("Confirm closed"), "Confirm closed");
    expect($("queue-finished")!.textContent).toContain("Queue complete · 2 of 2 closed");
    expect([stateOf("S1"), stateOf("S2")]).toEqual(["Closed", "Closed"]);
    expect([$("queue-file-S1")!.textContent, $("queue-file-S2")!.textContent]).toEqual(["s1.h5", "s2.h5"]);
    await advance(10_000);
    expect(acted()).toBe(4);
    // New plan: a new experiment id, the queue format kept.
    await press(button("New plan"), "New plan");
    expect(experimentId()).not.toBe(id);
    expect($("queue-card")).not.toBeNull();
  });

  it("a run that ends without Next sample (the classic Stop) is followed; the next sample still waits for its Start", async () => {
    await twoSamples();
    await press($("queue-start"), "Start S1");
    await report({ state: S.Active, start_generation: "1", output_path: "/data/s1.h5" });
    await report({ state: S.Idle, terminal: true, finalization_ok: true, start_generation: "1", output_path: "/data/s1.h5" });
    expect(button("Confirm closed")).toBeDefined();
    await press(button("Confirm closed"), "Confirm closed");
    await advance(10_000);
    expect(runStart).toHaveBeenCalledTimes(1);
    expect(stop).not.toHaveBeenCalled();
  });

  it("Start is blocked with the reason until the status has been read", async () => {
    await twoSamples();
    await act(async () => setStatus(null));
    expect($("queue-start")!.hasAttribute("disabled")).toBe(true);
    expect($("plan-blocked-status")!.textContent).toContain("run status not read yet");
    await press($("queue-start"), "Start S1");
    expect(acted()).toBe(0);
  });
});

describe("a failure stops the flow at its step, with the operator's choice", () => {
  it("a refused Start (InvalidRequest): the reason, nothing recorded, the row editable, Start again with the same index", async () => {
    runStart = vi.fn(async () => ({ kind: "refused", path: "/data/s1.h5", message: "invalid start request: annotation.condition: 300 bytes, at most 256" }) as PlanStartResult);
    await twoSamples();
    await press($("queue-start"), "Start S1");
    expect($("queue-step")!.textContent).toContain("Condition: 300 bytes, at most 256. No file was created.");
    expect(stateOf("S1")).toBe("Not started");
    expect(stepStates()[1]).toBe("failed");
    await selectRow("S1");
    expect(($("plan-condition") as HTMLInputElement).disabled).toBe(false);
    expect(button("Skip S1")).toBeDefined();
    expect(button("End queue")).toBeDefined();
    runStart = vi.fn(async (_json: string): Promise<PlanStartResult> => ({ kind: "started", path: "/data/s1.h5", notice: "" }));
    await render(); // the hook keeps its state; the new mock answers
    await press(button("Start S1"), "Start S1");
    expect(JSON.parse(runStart.mock.calls[0][0]).run_index).toBe(1);
  });

  it("a lost start reply: unknown, locked and visible; Retry sends a new run index, never the same one", async () => {
    runStart = vi.fn(async () => ({ kind: "unknown", path: "/data/s1.h5", message: "TRANSPORT_LOST" }) as PlanStartResult);
    await twoSamples();
    await press($("queue-start"), "Start S1");
    expect(stateOf("S1")).toBe("Unknown · reply lost");
    expect($("queue-step")!.textContent).toContain("Start outcome not known · S1, run 1");
    expect(host.querySelector("[data-testid=plan-sheet-table] tbody tr")!.children[0].textContent).toBe("1?");
    await selectRow("S1");
    expect(($("plan-condition") as HTMLInputElement).disabled).toBe(true);
    expect($("queue-next-run-index")!.textContent).toBe("2");
    await press(button("Retry S1 as run 2"), "Retry");
    expect(runStart).toHaveBeenCalledTimes(2);
    expect(JSON.parse(runStart.mock.calls[1][0])).toMatchObject({ run_index: 2, sample_ref: "S1" });
    // Lost again: the next retry is run 3. Skip moves on to S2, which gets run 3.
    await press(button("Skip S1"), "Skip S1");
    expect(stateOf("S1")).toBe("Skipped · after a run");
    expect($("queue-step")!.textContent).toContain("Swap the tube for S2");
    runStart = vi.fn(async (_json: string): Promise<PlanStartResult> => ({ kind: "started", path: "/data/s2.h5", notice: "" }));
    await render();
    await press(button("Start S2"), "Start S2");
    expect(JSON.parse(runStart.mock.calls[0][0])).toMatchObject({ run_index: 3, sample_ref: "S2" });
  });

  it("a stop that is not accepted: the reason, the run shown running; Next sample sends it again; End queue says the run keeps running", async () => {
    stop = vi.fn(async () => ({ ok: false, message: "Busy" }));
    await twoSamples();
    await press($("queue-start"), "Start S1");
    await report({ state: S.Active, start_generation: "1" });
    await press(button("Next sample"), "Next sample");
    expect($("queue-step")!.textContent).toContain("S1 · the stop was not accepted");
    expect(stateOf("S1")).toBe("Running · stop not accepted");
    await press(button("Next sample"), "Next sample");
    expect(stop).toHaveBeenCalledTimes(2);
    await press(button("End queue"), "End queue");
    expect($("queue-finished")!.textContent).toContain("A run is still active: stop it in the classic view.");
    expect(stop).toHaveBeenCalledTimes(2);
  });

  it("a failed run: the reason and the kept file; retry as a new run, skip or end", async () => {
    await twoSamples();
    await press($("queue-start"), "Start S1");
    await report({ state: S.Active, start_generation: "1", output_path: "/data/s1.h5" });
    await report({ state: S.Failed, terminal: true, finalization_ok: false, start_generation: "1", output_path: "/data/s1.h5", fault_code: "experiment.saveFailed", fault_message: "disk full" });
    expect(stateOf("S1")).toBe("Failed");
    expect($("queue-step")!.textContent).toContain("S1 · run 1 failed");
    expect($("queue-step")!.textContent).toContain("disk full");
    expect($("queue-step")!.textContent).toContain("The file is kept: /data/s1.h5.");
    expect(button("Confirm closed")).toBeUndefined();
    await press(button("Retry S1 as run 2"), "Retry");
    expect(JSON.parse(runStart.mock.calls[1][0])).toMatchObject({ run_index: 2, sample_ref: "S1" });
  });
});

describe("a lost controlling client", () => {
  it("pauses the flow; nothing can be pressed; the run's end is still shown; Resume needs control again and sends nothing", async () => {
    await twoSamples();
    await press($("queue-start"), "Start S1");
    await report({ state: S.Active, start_generation: "1", output_path: "/data/s1.h5" });
    await act(async () => setControl(false));
    expect($("queue-paused")!.textContent).toContain("Queue paused · control lost");
    expect($("queue-progress")!.textContent).toBe("Paused");
    expect(button("Next sample")!.disabled).toBe(true);
    await press(button("Next sample"), "Next sample");
    expect(stop).not.toHaveBeenCalled();
    // The server stops and saves the run (#711): the end is shown, the close is not confirmed for the operator.
    await report({ state: S.Idle, terminal: true, finalization_ok: true, start_generation: "1", output_path: "/data/s1.h5" });
    expect(button("Confirm closed")!.disabled).toBe(true);
    expect(button("Resume queue")!.disabled).toBe(true);
    // Control comes back: still paused until the operator resumes; resuming sends nothing.
    await act(async () => setControl(true));
    await advance(10_000);
    expect($("queue-paused")).not.toBeNull();
    expect(button("Confirm closed")!.disabled).toBe(true);
    await press(button("Resume queue"), "Resume queue");
    expect($("queue-paused")).toBeNull();
    expect(acted()).toBe(1);
    await press(button("Confirm closed"), "Confirm closed");
    expect($("queue-step")!.textContent).toContain("Swap the tube for S2");
    await advance(10_000);
    expect(acted()).toBe(1); // S2 waits for its Start
  });

  it("without control, Start says why", async () => {
    await twoSamples();
    await act(async () => setControl(false));
    expect($("queue-paused")).toBeNull(); // nothing began: nothing to pause
    expect($("plan-blocked-control")!.textContent).toContain("this client does not control the instrument");
    await press($("queue-start"), "Start S1");
    expect(acted()).toBe(0);
  });
});

describe("the PZ7035 Run switch", () => {
  const facts = { instrumentModes: true, mode: "align", windowPlaced: true, experimentActive: false, runFrozen: false, modeError: "" };

  it("is a button: shown outside Run, pressed once it calls the switch once; navigation alone never calls it", async () => {
    await render({ runMode: planRunMode(facts) });
    const notice = $("plan-run-mode")!;
    expect(notice.textContent).toContain("Camera · Align · not in Run");
    await advance(5000);
    expect(onSwitchToRun).not.toHaveBeenCalled();
    await press(button("Switch to Run"), "Switch to Run");
    expect(onSwitchToRun).toHaveBeenCalledTimes(1);
    // In the queue too.
    await press(host.querySelectorAll<HTMLInputElement>(".mib-option input")[1], "Sample queue");
    expect($("queue-card")!.querySelector("[data-testid=plan-run-mode]")).not.toBeNull();
  });

  it("says why it cannot switch (no Run window placed, a run active), and is not offered in Run or off the PZ7035", async () => {
    await render({ runMode: planRunMode({ ...facts, windowPlaced: false }) });
    expect(button("Switch to Run")!.disabled).toBe(true);
    expect($("plan-run-mode")!.textContent).toContain("Switch to Run is unavailable: place the 512×96 Run window in Align first");
    expect(planRunMode({ ...facts, experimentActive: true })!.blocked).toBe("a run is active");
    expect(planRunMode({ ...facts, mode: "run" })).toBeNull();
    expect(planRunMode({ ...facts, instrumentModes: false })).toBeNull();
    expect(planRunMode({ ...facts, modeError: "producer busy" })).toMatchObject({ label: "Run refused · producer busy", tone: "blocked" });
  });
});
