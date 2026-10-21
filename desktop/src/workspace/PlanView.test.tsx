// @vitest-environment jsdom
// O3 Experiment · Plan (#716 A5) in the workspace, with the real plan state (usePlan) over a mocked
// Start flow: Start run sends the plan's exact startRequestJson and nothing else acts; the
// backend's InvalidRequest is shown next to the sheet in plain words and the plan is kept; over-
// bound values are refused before anything is sent; Prepare's checklist and the classic Start's
// locks disable Start run with their own words; a started run locks the plan.
import { act, StrictMode, useState } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { invoke } from "@tauri-apps/api/core";
import { EXPERIMENT_START_OUTCOMES as O, EXPERIMENT_START_REQUEST as B, JSON_TRANSPORT, READINESS_GATE_STATUSES as G } from "../bridgeContract";
import { startExperiment } from "../experimentStart";
import type { PreflightInput } from "../preflight";
import type { AlignFacts } from "./alignControls";
import type { ExperimentSubView } from "./ExperimentView";
import { UUID_V4, buildStartRequest } from "./planModel";
import { OPERATORS_STORAGE_KEY, usePlan, type PlanController, type PlanStartResult } from "./usePlan";
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

let host: HTMLDivElement, root: Root;
let runStart = vi.fn(async (_json: string): Promise<PlanStartResult> => ({ kind: "started", path: "/data/run.h5", notice: "" }));
const onExit = vi.fn(), onStage = vi.fn();

let planRef: PlanController | null = null;
function Harness({ over = {}, lockedReason = "", controlHeld = true }: { over?: Partial<WorkspaceProps>; lockedReason?: string; controlHeld?: boolean }) {
  const plan = usePlan(runStart, { controlHeld });
  planRef = plan;
  const [view, setView] = useState<ExperimentSubView>("Plan");
  const props: WorkspaceProps = {
    brand: "MIB Studio", source: "Mock camera", method: "", stage: 2, onStage, onExit, serviceMode: false,
    preflightInput: READY_INPUT, zStage: null, signature: "sig", confirmedFor: "", onConfirm: () => {}, confirmLockedReason: "",
    experimentActive: false, controlHeld, onRefreshChecks: () => {}, onOpenSetup: () => {},
    align: { facts: FACTS, subscribeFrames: () => () => {}, onSwitchMode: async () => {}, writeRoi: vi.fn(), lockedReason: "" },
    experiment: { plan, lockedReason, view, onView: setView, live: { snapshot: null, running: false, experimentActive: false, hostProcessing: true, subscribeFrames: () => {}, onStopAcquisition: async () => {}, onStopRun: async () => ({ ok: false, message: "not in this test" }) } }, ...over,
  };
  return <Workspace {...props} />;
}

const render = (props: { over?: Partial<WorkspaceProps>; lockedReason?: string; controlHeld?: boolean } = {}) =>
  act(async () => root.render(<StrictMode><Harness {...props} /></StrictMode>));
const advance = (ms: number) => act(async () => { await vi.advanceTimersByTimeAsync(ms); });
const $ = (id: string) => host.querySelector<HTMLElement>(`[data-testid=${id}]`);
const button = (text: string) => [...host.querySelectorAll("button")].find((b) => b.textContent === text) as HTMLButtonElement | undefined;
const startButton = () => button("Start run") ?? button("Starting…");
async function type(testId: string, value: string) {
  const el = host.querySelector<HTMLInputElement | HTMLTextAreaElement>(`[data-testid=${testId}]`);
  if (!el) throw new Error(`no field ${testId}`);
  const proto = el instanceof HTMLTextAreaElement ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
  await act(async () => {
    Object.getOwnPropertyDescriptor(proto, "value")!.set!.call(el, value);
    el.dispatchEvent(new Event("input", { bubbles: true }));
  });
}
async function typeAria(label: string, value: string) {
  const el = host.querySelector<HTMLInputElement>(`input[aria-label="${label}"]`)!;
  await act(async () => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(el, value);
    el.dispatchEvent(new Event("input", { bubbles: true }));
  });
}
const experimentId = () => $("plan-experiment-id")!.textContent!;
const backendCalls = () => vi.mocked(invoke).mock.calls.map(([cmd, args]) => ({ cmd, args }));

async function fillFull() {
  await type("plan-sample", "Donor 1 · vehicle");
  await type("plan-condition", "Vehicle");
  await type("plan-experimentalUnit", "D1");
  await type("plan-concentrationValue", "0.1");
  await type("plan-concentrationUnit", "µM");
  await type("plan-channel", "C1 · 20 µm");
  await type("plan-medium", "PBS + 0.5% MC");
  await type("plan-temperatureC", "22.5");
  await type("plan-notes", "Line 1\nLine 2");
  await act(async () => button("Add key/value")!.click());
  await typeAria("Key 1", "passage");
  await typeAria("Value 1", "12");
  await type("plan-operator-name", "A. Operator");
  await type("plan-operator-id", "op-7");
}

/** What the Plan reads: Prepare's path-less readiness and the calibration list (#716 A4b). */
const isPrepareRead = (c: { cmd: string; args: unknown }) =>
  (c.cmd === "fetch_experiment_readiness" && JSON.stringify(c.args) === JSON.stringify({ outputPath: "" })) || (c.cmd === "calibration_records_list" && c.args === undefined);
const calRecordsList = (inEffect: string | null) => ({ valid: true, store: { revision: 2, max_versions: 2048, error: null }, applied_record_id: inEffect,
  in_effect_record_id: inEffect, current_factor: 0.5, current_identity: { camera: "mock:Camera 01", roi: "full", chip: "not tracked", objective: "not tracked", known: true },
  records: [] });
let calList: unknown = calRecordsList(null);

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  calList = calRecordsList(null);
  vi.useFakeTimers();
  vi.resetAllMocks();
  runStart = vi.fn(async (_json: string): Promise<PlanStartResult> => ({ kind: "started", path: "/data/run.h5", notice: "" }));
  const saved = new Map<string, string>();
  vi.stubGlobal("localStorage", { getItem: (k: string) => saved.get(k) ?? null, setItem: (k: string, v: string) => void saved.set(k, v), removeItem: (k: string) => void saved.delete(k) });
  vi.mocked(invoke).mockImplementation(async (cmd: string) => {
    if (cmd === "fetch_experiment_readiness") return { transport_version: JSON_TRANSPORT.version, valid: true, ready: false, generation: "1",
      gates: [{ id: "storage.output", status: G.Unavailable, reason: "no output path chosen yet", remediation: "choose an HDF5 destination" }] };
    if (cmd === "calibration_records_list") return calList;
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

describe("the Plan acts only on Start run", () => {
  it("opening and filling the plan sends nothing but Prepare's path-less readiness reads", async () => {
    await render();
    expect(experimentId()).toMatch(UUID_V4);
    await fillFull();
    await advance(5000);
    for (const c of backendCalls()) expect(isPrepareRead(c)).toBe(true);
    expect(runStart).not.toHaveBeenCalled();
    // The sheet's row as it will be recorded: one colour per condition.
    const table = $("plan-sheet-table")!;
    expect(table.querySelector(".mib-condition")!.textContent).toBe("Vehicle");
    expect(table.textContent).toContain("0.1 µM");
  });

  it("Start run passes the plan's startRequestJson, exactly, once", async () => {
    await render();
    const id = experimentId();
    await fillFull();
    expect(startButton()!.disabled).toBe(false);
    await act(async () => startButton()!.click());
    expect(runStart).toHaveBeenCalledTimes(1);
    expect(runStart.mock.calls[0][0]).toBe(
      `{"experiment_id":"${id}","run_index":1,"sample_ref":"S1","operator":{"id":"op-7","name":"A. Operator"},` +
      `"annotation":{"sample":"Donor 1 · vehicle","condition":"Vehicle","experimental_unit":"D1","concentration":{"value":0.1,"unit":"µM"},` +
      `"channel":"C1 · 20 µm","medium":"PBS + 0.5% MC","temperature_c":22.5,"notes":"Line 1\\nLine 2","free":{"passage":"12"}}}`,
    );
  });

  it("a viewer's Start run is disabled: another tab or client holds control, and nothing is sent (#766 review)", async () => {
    await render({ controlHeld: false });
    await fillFull();
    const gate = $("plan-blocked-control")!;
    expect(gate.textContent).toContain("Another tab or client holds control");
    expect(gate.textContent).toContain("nothing is sent");
    expect(startButton()!.disabled).toBe(true);
    await act(async () => startButton()!.click());
    // Not even through the plan itself: the Start flow is not entered (no file dialog, no read).
    await act(async () => { expect(await planRef!.start(buildStartRequest(planRef!.draft, 0, 1).json!, 1)).toBeNull(); });
    expect(runStart).not.toHaveBeenCalled();
    for (const c of backendCalls()) expect(isPrepareRead(c)).toBe(true);
    // Control taken here: the gate goes and Start run sends the plan.
    await render({ controlHeld: true });
    expect($("plan-blocked-control")).toBeNull();
    expect(startButton()!.disabled).toBe(false);
    await act(async () => startButton()!.click());
    expect(runStart).toHaveBeenCalledTimes(1);
  });

  it("an empty plan sends its identity only", async () => {
    await render();
    await act(async () => startButton()!.click());
    expect(runStart.mock.calls[0][0]).toBe(`{"experiment_id":"${experimentId()}","run_index":1,"sample_ref":"S1"}`);
  });
});

describe("refusals keep the plan", () => {
  it("an over-bound value is refused next to its field with the limit named; nothing is sent", async () => {
    await render();
    await type("plan-condition", "x".repeat(B.max_text_bytes + 1));
    const field = $("plan-condition")!;
    expect(field.getAttribute("aria-invalid")).toBe("true");
    expect(field.parentElement!.querySelector(".ws-plan-issue")!.textContent).toBe(`Too long: ${B.max_text_bytes + 1} bytes, at most ${B.max_text_bytes}. Shorten it; nothing is cut off.`);
    expect((field as HTMLInputElement).value).toHaveLength(B.max_text_bytes + 1); // never cut off
    expect(startButton()!.disabled).toBe(true);
    expect($("plan-blocked-sheet")!.textContent).toContain(`Condition: too long: ${B.max_text_bytes + 1} bytes, at most ${B.max_text_bytes}`);
    await act(async () => startButton()!.click());
    expect(runStart).not.toHaveBeenCalled();
    await type("plan-condition", "x".repeat(B.max_text_bytes));
    expect(startButton()!.disabled).toBe(false);
  });

  it("the backend's InvalidRequest is shown in plain words next to the sheet; no run is claimed and the plan is kept", async () => {
    runStart = vi.fn(async () => ({ kind: "refused", path: "/data/run.h5", message: "invalid start request: annotation.condition: 300 bytes, at most 256" }) as PlanStartResult);
    await render();
    const id = experimentId();
    await fillFull();
    await act(async () => startButton()!.click());
    const refusal = $("plan-refusal")!;
    expect(refusal.closest("[data-testid=plan-sheet]")).not.toBeNull(); // next to the sheet
    expect(refusal.textContent).toContain("Not started · the backend refused the plan");
    expect(refusal.textContent).toContain("Condition: 300 bytes, at most 256. No file was created. The plan is kept: correct it and press Start run again.");
    expect(refusal.querySelector(".ws-gate-id")!.textContent).toBe("annotation.condition");
    // No run is claimed: no started notice, no lock, the same plan and experiment id.
    expect(host.textContent).not.toMatch(/Run 1 started|run has started/);
    expect($("plan-outcome")).toBeNull();
    expect(button("New plan")).toBeUndefined();
    expect(experimentId()).toBe(id);
    expect(($("plan-condition") as HTMLInputElement).value).toBe("Vehicle");
    expect(($("plan-condition") as HTMLInputElement).disabled).toBe(false);
    expect(startButton()!.disabled).toBe(false);
    expect(localStorage.getItem(OPERATORS_STORAGE_KEY)).toBeNull(); // nothing remembered for a refused start
  });

  it("other outcomes are said as they are, and keep the plan too", async () => {
    for (const [result, title] of [
      [{ kind: "not-ready", message: "/data/run.h5: storage.roundtrip: write test failed" }, "Not started · readiness with the chosen file"],
      [{ kind: "refused", path: "/data/run.h5", message: "Experiment is already running" }, "Not started · the backend refused the Start"],
      [{ kind: "error", message: "Error: dialog unavailable" }, "Not started · an error before the start was sent"],
    ] as const) {
      runStart = vi.fn(async () => result as PlanStartResult);
      await act(async () => root.unmount());
      root = createRoot(host);
      await render();
      const id = experimentId();
      await act(async () => startButton()!.click());
      expect($("plan-outcome")!.textContent).toContain(title);
      expect($("plan-refusal")).toBeNull();
      expect(experimentId()).toBe(id);
      expect(startButton()!.disabled).toBe(false);
    }
  });

  it("a cancelled dialog says nothing and changes nothing", async () => {
    runStart = vi.fn(async () => ({ kind: "cancelled" }) as PlanStartResult);
    await render();
    const id = experimentId();
    await act(async () => startButton()!.click());
    expect($("plan-outcome")).toBeNull();
    expect(experimentId()).toBe(id);
  });
});

describe("Start run follows Prepare and the classic Start's locks", () => {
  it("while Prepare's checklist blocks, Start run is disabled with Prepare's own words", async () => {
    const blocked: PreflightInput = { ...READY_INPUT, coreValid: false };
    await render({ over: { preflightInput: blocked, stage: 0 } });
    await advance(0);
    const prepareWhy = $("workspace-confirm-why")!.textContent!; // Prepare's "Confirm readiness is unavailable"
    const prepareVerdict = $("workspace-verdict")!.textContent!;
    await render({ over: { preflightInput: blocked, stage: 2 } });
    await advance(0);
    expect(startButton()!.disabled).toBe(true);
    const notice = $("plan-blocked-prepare")!;
    expect(notice.querySelector(".mib-notice__title")!.textContent).toBe(`Start is unavailable · Prepare: ${prepareVerdict}`);
    expect(prepareWhy).toContain(notice.querySelector(".mib-notice__detail")!.textContent!);
    expect(notice.querySelector(".mib-notice__detail")!.textContent).toBe("1 required check failed: Processing core / trust.");
    expect($("workspace-verdict")!.textContent).toBe(prepareVerdict); // the same verdict in the header
    await act(async () => startButton()!.click());
    expect(runStart).not.toHaveBeenCalled();
  });

  it("the classic Start button's reason disables it too, said next to it", async () => {
    await render({ lockedReason: "Camera must be running before starting an experiment" });
    expect(startButton()!.disabled).toBe(true);
    expect($("plan-blocked-locked")!.textContent).toContain("Camera must be running before starting an experiment.");
  });
});

describe("a started run locks its plan", () => {
  it("is decided by the start reply alone: a failed status refresh afterwards keeps the plan locked and is said apart (#736 review)", async () => {
    runStart = vi.fn(async () => ({ kind: "started", path: "/data/run.h5", notice: "", refreshError: "Error: TRANSPORT_LOST" }) as PlanStartResult);
    await render();
    await type("plan-condition", "Vehicle");
    await act(async () => startButton()!.click());
    expect($("plan-outcome")!.textContent).toContain("Run 1 started · /data/run.h5");
    expect($("plan-refresh")!.textContent).toContain("Run status not refreshed");
    expect($("plan-refresh")!.textContent).toContain("Error: TRANSPORT_LOST");
    expect(startButton()!.disabled).toBe(true);
    expect(($("plan-condition") as HTMLInputElement).disabled).toBe(true);
    await act(async () => startButton()!.click());
    expect(runStart).toHaveBeenCalledTimes(1);
  });

  it("a start whose reply was lost locks the plan too: the run may exist, so its identity is not sent again", async () => {
    runStart = vi.fn(async () => ({ kind: "unknown", path: "/data/run.h5", message: "TRANSPORT_LOST" }) as PlanStartResult);
    await render();
    const id = experimentId();
    await act(async () => startButton()!.click());
    expect($("plan-outcome")!.textContent).toContain("Start outcome not known · /data/run.h5");
    expect($("plan-blocked-started")!.textContent).toContain("This plan's identity may be recorded");
    expect(startButton()!.disabled).toBe(true);
    await act(async () => button("New plan")!.click());
    expect(experimentId()).not.toBe(id);
    expect(startButton()!.disabled).toBe(false);
  });

  it("an unexpected throw from the Start flow is treated as a lost reply", async () => {
    runStart = vi.fn(async () => { throw new Error("boom"); });
    await render();
    await act(async () => startButton()!.click());
    expect($("plan-outcome")!.textContent).toContain("Start outcome not known");
    expect(startButton()!.disabled).toBe(true);
  });

  it("locks the sheet, says where it is recorded, remembers the operator; New plan mints a new id", async () => {
    await render();
    const id = experimentId();
    await fillFull();
    await act(async () => startButton()!.click());
    expect($("plan-outcome")!.textContent).toContain("Run 1 started · /data/run.h5");
    expect(startButton()!.disabled).toBe(true);
    expect(($("plan-condition") as HTMLInputElement).disabled).toBe(true);
    expect($("plan-blocked-started")!.textContent).toContain("A new plan mints a new experiment id.");
    expect(JSON.parse(localStorage.getItem(OPERATORS_STORAGE_KEY)!)).toEqual([{ name: "A. Operator", id: "op-7" }]);
    await act(async () => button("Start run")!.click());
    expect(runStart).toHaveBeenCalledTimes(1); // no second run with the same identity

    await act(async () => button("New plan")!.click());
    expect(experimentId()).toMatch(UUID_V4);
    expect(experimentId()).not.toBe(id);
    expect(($("plan-condition") as HTMLInputElement).value).toBe("");
    expect(($("plan-operator-name") as HTMLInputElement).value).toBe("A. Operator"); // the selected operator stays
    expect($("plan-operators")!.textContent).toContain("A. Operator (op-7)");
    expect(startButton()!.disabled).toBe(false);
  });

  it("a remembered operator is only a suggestion: none is selected or sent until one is picked (#763 F6)", async () => {
    localStorage.setItem(OPERATORS_STORAGE_KEY, JSON.stringify([{ name: "B. Second", id: "" }, { name: "A. Operator", id: "op-7" }]));
    await render();
    // After a reload the plan names no operator: the remembered ones are offered, none chosen.
    expect(($("plan-operator-name") as HTMLInputElement).value).toBe("");
    expect(($("plan-operator-id") as HTMLInputElement).value).toBe("");
    const offered = [...$("plan-operators")!.querySelectorAll("button")];
    expect(offered.map((b) => b.textContent)).toEqual(["B. Second", "A. Operator (op-7)"]);
    expect(offered.every((b) => !b.className.includes("selected"))).toBe(true);
    expect($("plan-operator-note")!.textContent).toContain("a remembered name is never selected for you");
    await act(async () => startButton()!.click());
    expect(runStart).toHaveBeenCalledTimes(1);
    expect(JSON.parse(runStart.mock.calls[0][0])).not.toHaveProperty("operator"); // nothing recorded as "selected"
  });

  it("an operator picked from the remembered list is sent, and New plan keeps the one picked here", async () => {
    localStorage.setItem(OPERATORS_STORAGE_KEY, JSON.stringify([{ name: "B. Second", id: "" }, { name: "A. Operator", id: "op-7" }]));
    await render();
    await act(async () => button("A. Operator (op-7)")!.click());
    expect(($("plan-operator-id") as HTMLInputElement).value).toBe("op-7");
    expect($("plan-operator-note")!.textContent).toContain("not authentication");
    await act(async () => startButton()!.click());
    expect(JSON.parse(runStart.mock.calls[0][0]).operator).toEqual({ id: "op-7", name: "A. Operator" });
    await act(async () => button("New plan")!.click());
    expect(($("plan-operator-name") as HTMLInputElement).value).toBe("A. Operator"); // picked in this page's Plan
  });

  it("a remembered operator is never sent without a pick, whatever was remembered (#763 F6)", async () => {
    // The plan a page load creates: for any remembered list, the first Start sends no operator.
    for (const remembered of [[{ name: "Solo", id: "" }], [{ name: "", id: "id-only" }], [{ name: "X", id: "x-1" }, { name: "Y", id: "" }]]) {
      localStorage.setItem(OPERATORS_STORAGE_KEY, JSON.stringify(remembered));
      runStart.mockClear();
      await act(async () => root.unmount());
      root = createRoot(host);
      await render();
      await act(async () => startButton()!.click());
      expect(runStart).toHaveBeenCalledTimes(1);
      expect(runStart.mock.calls[0][0]).not.toContain("\"operator\"");
    }
  });
});

describe("click-through polish (#763)", () => {
  it("F12: a started run's outcome points to Live, and Go to Live only shows it", async () => {
    await render();
    await act(async () => startButton()!.click());
    const outcome = $("plan-outcome")!;
    expect(outcome.textContent).toContain("Follow it in Experiment · Live, and stop it there with Stop & finalize run.");
    expect(outcome.textContent).not.toContain("classic view");
    const sent = backendCalls().length;
    await act(async () => $("plan-to-live")!.click());
    expect($("workspace-live")).not.toBeNull();
    // Showing Live reads (and, from the controlling client, turns monitoring on); it starts or stops nothing.
    expect(backendCalls().slice(sent).every((c) => !["experiment_start", "experiment_stop", "stop_camera"].includes(c.cmd))).toBe(true);
  });

  it("F21: each issue is its own message, under its field and in the Start block", async () => {
    await render();
    await type("plan-concentrationValue", "-1");
    const field = $("plan-concentrationValue")!;
    const lines = [...field.parentElement!.querySelectorAll(".ws-plan-issue__line")].map((l) => l.textContent);
    expect(lines).toEqual(["Give a unit with the value (e.g. µM), or clear the value when it is not declared.", "Must be 0 or more."]);
    const items = [...$("plan-blocked-sheet")!.querySelectorAll("li")].map((l) => l.textContent);
    expect(items).toEqual([
      "Concentration: give a unit with the value (e.g. µM), or clear the value when it is not declared.",
      "Concentration value: must be 0 or more.",
    ]);
  });

  it("F23: Start says the preflight is not confirmed in this browser tab, and stays available", async () => {
    await render();
    expect($("plan-confirmation")!.textContent).toBe("Not confirmed in this browser tab");
    expect($("plan-confirmation-note")!.textContent).toContain("advisory");
    expect(startButton()!.disabled).toBe(false); // advisory: Start is not gated on it
    await render({ over: { confirmedFor: "sig" } });
    expect($("plan-confirmation")).toBeNull();
    expect($("plan-confirmation-note")).toBeNull();
    expect(startButton()!.disabled).toBe(false);
    await render({ over: { confirmedFor: "sig-before" } });
    expect($("plan-confirmation")!.textContent).toBe("Setup changed since the confirmation");
  });
});

describe("the sample format", () => {
  it("offers a single sample (chosen) and the sample queue (#716 A5, QueuePlan.test.tsx); choosing sends nothing", async () => {
    await render();
    const options = [...host.querySelectorAll<HTMLLabelElement>(".mib-option")];
    expect(options.map((o) => o.querySelector(".mib-option__label")!.textContent)).toEqual(["Single sample", "Sample queue"]);
    expect(options[0].querySelector("input")!.checked).toBe(true);
    const queue = options[1].querySelector("input")!;
    expect(queue.disabled).toBe(false);
    expect(options[1].querySelector(".mib-option__reason")).toBeNull();
    await act(async () => queue.click());
    expect($("queue-card")).not.toBeNull();
    expect(runStart).not.toHaveBeenCalled();
  });

  it("is fixed once the single sample's run has started", async () => {
    await render();
    await act(async () => startButton()!.click());
    expect(host.querySelectorAll<HTMLInputElement>(".mib-option input")[1].disabled).toBe(true);
  });
});

it("buildStartRequest is what the view sends (no second serializer)", async () => {
  await render();
  await type("plan-condition", "Vehicle");
  await act(async () => startButton()!.click());
  const sent = runStart.mock.calls[0][0];
  expect(sent).toBe(buildStartRequest({ experimentId: JSON.parse(sent).experiment_id, format: "single", operator: { name: "", id: "" },
    rows: [{ ref: "S1", sample: "", condition: "Vehicle", experimentalUnit: "", concentrationValue: "", concentrationUnit: "", channel: "", medium: "", temperatureC: "", notes: "", free: [] }] }).json);
});

// Bridge ABI 40 (#741): the start reply's code, through the real Start flow, decides the single plan.
describe("the start outcome code (ABI 40)", () => {
  const startAnswering = (code: number | undefined, message: string) => vi.fn(async (json: string, stillWanted?: () => boolean): Promise<PlanStartResult> => startExperiment({
    fetchRingStatus: async () => null, confirm: async () => true, pickFile: async () => "/data/run.h5",
    fetchReadiness: async () => ({ valid: true, ready: true, gates: [] }),
    start: async () => ({ ok: code === O.Started, message, ...(code === undefined ? {} : { start_outcome: code }) }),
  }, json, stillWanted));

  it.each([["NotReady", O.NotReady], ["StorageFailed", O.StorageFailed], ["ProvenanceFailed", O.ProvenanceFailed]] as const)(
    "%s locks the plan: the backend may have opened the file, so the identity is not sent again", async (name, code) => {
      runStart = startAnswering(code, `failed ${name}`);
      await render();
      const id = experimentId();
      await type("plan-condition", "Vehicle");
      await act(async () => startButton()!.click());
      expect($("plan-outcome")!.textContent).toContain("Not started · its file may exist · /data/run.h5");
      expect($("plan-outcome")!.textContent).toContain(`The backend did not start the run (failed ${name})`);
      expect($("plan-blocked-started")!.textContent).toContain("it may have opened the run's file first");
      expect($("plan-refusal")).toBeNull();
      expect(startButton()!.disabled).toBe(true);
      expect(($("plan-condition") as HTMLInputElement).disabled).toBe(true);
      await act(async () => startButton()!.click());
      expect(runStart).toHaveBeenCalledTimes(1);
      await act(async () => button("New plan")!.click());
      expect(experimentId()).not.toBe(id);
    });

  it.each([["StaleReadiness", O.StaleReadiness], ["AlreadyActive", O.AlreadyActive], ["Busy", O.Busy]] as const)(
    "%s refuses before anything is recorded: the plan is kept, even when the message reads like an invalid request", async (name, code) => {
      runStart = startAnswering(code, `invalid start request: not this, ${name}`);
      await render();
      const id = experimentId();
      await act(async () => startButton()!.click());
      expect($("plan-outcome")!.textContent).toContain("Not started · the backend refused the Start");
      expect($("plan-refusal")).toBeNull(); // the code decides, not the message
      expect(experimentId()).toBe(id);
      expect(startButton()!.disabled).toBe(false);
    });

  it("InvalidRequest by its code is shown next to the sheet, with or without the message prefix", async () => {
    runStart = startAnswering(O.InvalidRequest, "startRequest: unknown key \"operator_name\"");
    await render();
    await act(async () => startButton()!.click());
    expect($("plan-refusal")!.textContent).toContain('The plan as sent: startRequest: unknown key "operator_name". No file was created.');
    expect($("plan-outcome")).toBeNull();
    expect(startButton()!.disabled).toBe(false);
  });

  it("without a code (a backend before ABI 40) the message prefix still decides", async () => {
    runStart = startAnswering(undefined, "invalid start request: annotation.condition: 300 bytes, at most 256");
    await render();
    await act(async () => startButton()!.click());
    expect($("plan-refusal")!.textContent).toContain("Condition: 300 bytes, at most 256. No file was created.");
    runStart = startAnswering(undefined, "Experiment is already running");
    await act(async () => root.unmount());
    root = createRoot(host);
    await render();
    await act(async () => startButton()!.click());
    expect($("plan-outcome")!.textContent).toContain("Not started · the backend refused the Start");
    expect(startButton()!.disabled).toBe(false);
  });
});

// #716 A4b: the Plan declares calibration_record_id only as the record it showed as in effect.
describe("the calibration record a Start declares (A4b)", () => {
  const MISMATCH = 'invalid start request: calibration_record_id: declared "cal-a" but the applied record is "cal-b" (mismatch)';

  it("the record shown as in effect is declared at Start, and shown in the run identity", async () => {
    calList = calRecordsList("cal-a");
    await render();
    await advance(0);
    expect($("plan-calibration-declared")!.textContent).toContain("cal-a · in effect (0.5 µm/px) · declared at Start");
    await act(async () => startButton()!.click());
    expect(runStart).toHaveBeenCalledTimes(1);
    expect(runStart.mock.calls[0][0]).toBe(`{"experiment_id":"${experimentId()}","run_index":1,"sample_ref":"S1","calibration_record_id":"cal-a"}`);
  });

  it("with none in effect, nothing is declared", async () => {
    calList = calRecordsList(null);
    await render();
    await advance(0);
    expect($("plan-identity")!.textContent).toContain("None in effect when last read · not declared; the backend records the record in effect at Start");
    await act(async () => startButton()!.click());
    expect(JSON.parse(runStart.mock.calls[0][0])).not.toHaveProperty("calibration_record_id");
  });

  it("with the list not read, nothing is declared and the record is unknown, never None", async () => {
    vi.mocked(invoke).mockImplementation(async (cmd: string) => {
      if (cmd === "fetch_experiment_readiness") return { transport_version: JSON_TRANSPORT.version, valid: true, ready: false, generation: "1",
        gates: [{ id: "storage.output", status: G.Unavailable, reason: "no output path chosen yet", remediation: "choose an HDF5 destination" }] };
      throw new Error("TRANSPORT_LOST");
    });
    await render();
    await advance(0);
    expect($("plan-identity")!.textContent).toContain("Unknown · Error: TRANSPORT_LOST · not declared");
    expect($("plan-identity")!.textContent).not.toContain("None in effect");
    await act(async () => startButton()!.click());
    expect(JSON.parse(runStart.mock.calls[0][0])).not.toHaveProperty("calibration_record_id");
  });

  it("the backend's mismatch refusal says the applied calibration changed since this page read it; the plan is kept and the list is read again", async () => {
    calList = calRecordsList("cal-a");
    runStart = vi.fn(async () => ({ kind: "refused", path: "/data/run.h5", message: MISMATCH, code: O.InvalidRequest }) as PlanStartResult);
    await render();
    await advance(0);
    const id = experimentId();
    const reads = () => backendCalls().filter((c) => c.cmd === "calibration_records_list").length;
    calList = calRecordsList("cal-b");
    const before = reads();
    await act(async () => startButton()!.click());
    await advance(0);
    const refusal = $("plan-calibration-refusal")!;
    expect(refusal.closest("[data-testid=plan-identity]")).not.toBeNull(); // beside the calibration record, not the sheet
    expect(refusal.textContent).toContain("Not started · the applied calibration changed since this page read it");
    expect(refusal.textContent).toContain('declared "cal-a" but the applied record is "cal-b" (mismatch). No file was created.');
    expect($("plan-refusal")).toBeNull();
    expect($("plan-outcome")).toBeNull();
    expect(reads()).toBeGreaterThan(before); // read again at once
    expect($("plan-calibration-declared")!.textContent).toContain("cal-b · in effect");
    // The plan is kept: the same identity, and the next Start declares what is shown now.
    expect(experimentId()).toBe(id);
    expect(startButton()!.disabled).toBe(false);
    await act(async () => startButton()!.click());
    expect(JSON.parse(runStart.mock.calls[1][0])).toMatchObject({ experiment_id: id, run_index: 1, calibration_record_id: "cal-b" });
  });

  it("with the calibration store unreadable, nothing is declared and the record is unknown (Codex round 1)", async () => {
    calList = { ...calRecordsList(null), store: { revision: 0, max_versions: 2048, error: "unreadable" } };
    await render();
    await advance(0);
    expect($("plan-identity")!.textContent).toContain("Unknown · the calibration store cannot be read · not declared");
    await act(async () => startButton()!.click());
    expect(JSON.parse(runStart.mock.calls[0][0])).not.toHaveProperty("calibration_record_id");
  });
});
