// @vitest-environment jsdom
// The opt-in workspace and its Prepare view over the mocked transport (#713): Prepare reads the
// backend gates without a path and nothing else, renders Unknown as unknown, and stages only
// change what is shown.
import { act, useState } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { invoke } from "@tauri-apps/api/core";
import { JSON_TRANSPORT, READINESS_GATE_STATUSES as G } from "../bridgeContract";
import type { ReadinessGate } from "../eventAdapter";
import type { StageStatus } from "../bridge";
import type { PreflightInput } from "../preflight";
import { derivePreflight } from "../preflight";
import { PREPARE_READINESS_POLL_MS, prepareVerdict } from "./prepareModel";
import { calList as calListOf, calRecord } from "./calibrationFixtures";
import { Workspace, type WorkspaceProps } from "./Workspace";
import type { AlignFacts } from "./alignControls";
import type { ExperimentSubView } from "./ExperimentView";
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
const NO_PATH: ReadinessGate = { id: "storage.output", status: G.Unavailable, reason: "no output path chosen yet", remediation: "choose an HDF5 destination" };
let gates: ReadinessGate[] = [{ id: "camera.source", status: G.Pass, reason: "", remediation: "" }, NO_PATH];
const readinessReply = () => ({ transport_version: JSON_TRANSPORT.version, valid: true, ready: false, generation: "3", gates });

let host: HTMLDivElement, root: Root;
const onStage = vi.fn(), onExit = vi.fn(), onConfirm = vi.fn(), onOpenSetup = vi.fn(), onRefreshChecks = vi.fn();
const FACTS: AlignFacts = {
  pz7035: false, instrumentModes: false, hostProcessing: true, geometry: null, instrumentMode: null, runFrozen: false, modeError: "",
  appliedRunWindow: null, placedRunWindow: null, processingRoi: null, runFps: null, experimentActive: false, running: false,
};
const onSwitchMode = vi.fn(async () => {}), writeRoi = vi.fn(), subscribeFrames = vi.fn();
const planStart = vi.fn(async () => null);
const PLAN: PlanController = {
  draft: newPlanDraft("3f2b8c1e-9a4d-4e6f-8b21-0c5d7e9f1a2b"), edit: () => {}, operators: [], result: null, started: null, starting: false,
  start: planStart, newPlan: () => {}, reference: null, setReference: () => {},
  queue: { state: newQueue(), pending: null, controlHeld: true, status: null, startCurrent: async () => {}, stopCurrent: async () => {},
    confirmClosed: () => {}, skip: () => {}, end: () => {}, resume: () => {}, repeat: () => {} },
};
const props = (over: Partial<WorkspaceProps> = {}): WorkspaceProps => ({
  brand: "MIB Studio", source: "Mock camera", method: "", stage: 0, onStage, onExit, serviceMode: false,
  preflightInput: INPUT, zStage: null, signature: "sig-now", confirmedFor: "", onConfirm, confirmLockedReason: "",
  experimentActive: false, controlHeld: true, onRefreshChecks, onOpenSetup,
  align: { facts: FACTS, subscribeFrames, onSwitchMode, writeRoi, lockedReason: "" },
  experiment: { plan: PLAN, lockedReason: "", view: "Plan", onView: () => {}, live: { snapshot: null, running: false, experimentActive: false, hostProcessing: true, subscribeFrames: () => {}, onStopAcquisition: async () => {}, onStopRun: async () => ({ ok: false, message: "not in this test" }) } }, ...over,
});
const onView = vi.fn();
/** The Experiment sub-view is held by App: here by a wrapper, as App does. */
function WithView(p: WorkspaceProps) {
  const [view, setView] = useState<ExperimentSubView>(p.experiment.view);
  return <Workspace {...p} experiment={{ ...p.experiment, view, onView: (v) => { setView(v); onView(v); } }} />;
}
const render = (over: Partial<WorkspaceProps> = {}) => act(async () => root.render(<WithView {...props(over)} />));
const advance = (ms: number) => act(async () => { await vi.advanceTimersByTimeAsync(ms); });
const calls = () => vi.mocked(invoke).mock.calls.map(([cmd, args]) => ({ cmd, args }));
/** What Prepare reads (and the Plan, which takes its verdict and declares its calibration record):
 *  the path-less readiness and the calibration records list (#716 A4b). Neither acts. */
const PREPARE_READ = [{ cmd: "fetch_experiment_readiness", args: { outputPath: "" } }, { cmd: "calibration_records_list", args: undefined }];
const isPrepareRead = (c: { cmd: string; args: unknown }) => PREPARE_READ.some((r) => r.cmd === c.cmd && JSON.stringify(r.args) === JSON.stringify(c.args));
const EMPTY_CAL_LIST = { valid: true, store: { revision: 0, max_versions: 2048, error: null }, applied_record_id: null, in_effect_record_id: null, current_factor: 0.5,
  current_identity: { camera: "mock:Camera 01", roi: "full", chip: "not tracked", objective: "not tracked", known: true }, records: [] };
let calList: unknown = EMPTY_CAL_LIST;
const button = (text: string) => [...host.querySelectorAll("button")].find((b) => b.textContent === text)!;
const row = (label: string, scope = "") => [...host.querySelectorAll(`${scope} dt`)].find((d) => d.textContent === label)?.nextElementSibling as HTMLElement | undefined;
const GATES = "[data-testid=workspace-gates]";
/** The value cell of a gate row, found by the gate id (the label is the plain name). */
const gateRow = (id: string, scope = GATES) => host.querySelector(`${scope} [data-gate-id="${id}"]`)?.closest("dt")?.nextElementSibling as HTMLElement | undefined;
const verdictChip = () => host.querySelector("[data-testid=workspace-verdict]")!;
/** The verdict card's chip: prepareVerdict's words, the same as the header chip. */
const cardChip = () => host.querySelector("[data-testid=workspace-prepare-verdict-chip]")!;

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  vi.useFakeTimers();
  vi.resetAllMocks();
  gates = [{ id: "camera.source", status: G.Pass, reason: "", remediation: "" }, NO_PATH];
  calList = EMPTY_CAL_LIST;
  vi.mocked(invoke).mockImplementation(async (cmd: string) => {
    if (cmd === "fetch_experiment_readiness") return readinessReply();
    if (cmd === "calibration_records_list") return calList;
    throw new Error(`unexpected command ${cmd}`);
  });
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => {
  await act(async () => root.unmount());
  host.remove();
  vi.useRealTimers();
});

describe("Prepare reads readiness without a path (#713)", () => {
  it("polls only the readiness, always with an empty output path (so no probe file is written), and the calibration list", async () => {
    await render();
    await advance(PREPARE_READINESS_POLL_MS * 5);
    const log = calls();
    const readiness = log.filter((c) => c.cmd === "fetch_experiment_readiness");
    expect(readiness.length).toBe(6); // the first read, then one per period
    for (const c of readiness) expect(c).toEqual({ cmd: "fetch_experiment_readiness", args: { outputPath: "" } });
    expect(log.filter((c) => c.cmd === "calibration_records_list").length).toBe(6); // the same cadence
    for (const c of log) expect(isPrepareRead(c)).toBe(true);
  });

  it("Refresh and Read gates again read without a path too", async () => {
    await render();
    await act(async () => button("Read gates again").click());
    await act(async () => button("Refresh").click());
    await advance(0);
    expect(calls().every(isPrepareRead)).toBe(true);
    expect(onRefreshChecks).toHaveBeenCalledTimes(1);
  });

  it("stops polling on another stage and while a run is active", async () => {
    await render();
    await render({ stage: 1 });
    const before = calls().length;
    await advance(PREPARE_READINESS_POLL_MS * 4);
    expect(calls().length).toBe(before);
    await render({ stage: 0, experimentActive: true });
    await advance(PREPARE_READINESS_POLL_MS * 4);
    expect(calls().length).toBe(before);
    expect(host.textContent).toContain("Readiness paused · a run is active");
  });
});

describe("Prepare states", () => {
  it("shows the gates read-only under plain labels, the destination as Unavailable, and one verdict", async () => {
    await render();
    const label = host.querySelector(`${GATES} [data-gate-id="storage.output"]`)!;
    expect(label.textContent).toBe("Destination filestorage.output"); // plain label, then the id for support
    expect(label.getAttribute("title")).toBe("storage.output");
    const value = gateRow("storage.output")!;
    const chip = value.querySelector(".mib-chip")!;
    expect(chip.textContent).toBe("Unavailable · no output path chosen yet");
    expect(chip.getAttribute("data-tone")).toBe("unknown");
    // Never counted: Prepare names no file, Start checks the chosen one (O1b).
    expect(value.textContent).toContain("Expected in Prepare: the file is chosen at Start. Never counted as an issue.");
    expect(value.querySelector(".ws-gate-fix")).toBeNull();
    expect(gateRow("storage.output", "[data-testid=workspace-prepare]")!.textContent).toContain("Unavailable · no output path chosen yet"); // the destination card
    // One verdict, the same words in the header and in the card.
    expect(verdictChip().textContent).toBe("Ready · destination checked at Start");
    expect(verdictChip().getAttribute("data-tone")).toBe("confirmed");
    expect(cardChip().textContent).toBe(verdictChip().textContent);
    expect(host.querySelector("[data-testid=workspace-footnote]")!.textContent).toBe("Unknown is never Pass. Unsupported never blocks. Start re-evaluates every gate with the chosen file.");
  });

  it("an unknown gate keeps its raw id as the label, never hidden", async () => {
    gates = [{ id: "future.gate", status: G.Warn, reason: "new", remediation: "" }, NO_PATH];
    await render();
    expect(host.querySelector(`${GATES} [data-gate-id="future.gate"]`)!.textContent).toBe("future.gate");
    expect(verdictChip().textContent).toBe("Ready with warnings · 1 warning");
  });

  it("is never a pass before the first read", async () => {
    vi.mocked(invoke).mockImplementation(() => new Promise(() => {})); // never answers
    await render();
    expect(verdictChip().textContent).toBe("Ready to confirm · gates not read yet");
    expect(verdictChip().getAttribute("data-tone")).toBe("unknown");
    expect(host.querySelector("[data-testid=workspace-start-refused]")).toBeNull();
  });

  it("counts blocking gates and checks in one verdict when the checklist blocks", async () => {
    gates = [{ id: "camera.source", status: G.Unavailable, reason: "no camera source configured", remediation: "" }, { id: "processing.core", status: G.Fail, reason: "pin", remediation: "" }, NO_PATH];
    await render({ preflightInput: { ...INPUT, coreKnown: false, coreValid: false } });
    expect(verdictChip().textContent).toBe("Not ready · 1 failed, 2 unknown");
    expect(verdictChip().getAttribute("data-tone")).toBe("blocked");
    expect(host.querySelector("[data-testid=workspace-verdict-detail]")!.textContent).toBe(
      "Failed: Processing core. Unknown: Processing core / trust, Camera source. The destination is checked at Start, with the chosen file.");
    expect(host.querySelector("[data-testid=workspace-start-refused]")).toBeNull(); // Confirm is unavailable; its Notice says why
  });

  it("checklist passes but a gate fails: Ready to confirm · Start blocked, Confirm stays available, and a Notice says what Start will refuse", async () => {
    gates = [{ id: "camera.source", status: G.Pass, reason: "", remediation: "" },
      { id: "processing.core", status: G.Fail, reason: "processing core pin not satisfied", remediation: "activate the pinned core" }, NO_PATH];
    await render();
    expect(verdictChip().textContent).toBe("Ready to confirm · Start blocked: Processing core");
    expect(verdictChip().getAttribute("data-tone")).toBe("warn");
    expect(cardChip().textContent).toBe(verdictChip().textContent);
    expect(cardChip().getAttribute("data-tone")).toBe("warn");
    expect(button("Confirm hardware preflight").disabled).toBe(false); // the checklist drives Confirm, as in the classic Preflight
    expect(host.querySelector("[data-testid=workspace-confirm-why]")).toBeNull();
    const refused = host.querySelector("[data-testid=workspace-start-refused]")!;
    // One line beside Confirm (O1b); the full list with each Fix is its title.
    expect(refused.textContent).toBe("Start is refused until Processing core passes.");
    expect(refused.getAttribute("title")).toBe("Start will be refused until: Processing core — activate the pinned core.");
    expect(host.textContent).not.toContain("Not ready");
  });

  it("a failed read clears the gates instead of showing stale ones", async () => {
    await render();
    expect(gateRow("camera.source")).toBeDefined();
    vi.mocked(invoke).mockRejectedValue(new Error("bridge gone"));
    await advance(PREPARE_READINESS_POLL_MS);
    expect(gateRow("camera.source")).toBeUndefined();
    expect(verdictChip().getAttribute("data-tone")).toBe("unknown");
  });

  it("renders a check not read yet as Unknown, not failed, and keeps Confirm disabled", async () => {
    await render({ preflightInput: { ...INPUT, coreKnown: false, coreValid: false } });
    const core = row("Processing core / trust · required")!;
    expect(core.textContent).toBe("Unknown · expected 2.1.0 · Processing-core status not read yet.");
    expect(core.className).toContain("mib-facts__value--unknown");
    expect(core.className).not.toContain("blocked");
    expect(host.textContent).toContain("Required checks: 1 of 2 ready · 1 unknown");
    expect(button("Confirm hardware preflight").disabled).toBe(true);
    // The disabled button says why, in plain words, next to it.
    const why = host.querySelector("[data-testid=workspace-confirm-why]")!;
    expect(why.getAttribute("data-tone")).toBe("unknown");
    expect(why.textContent).toBe("Confirm hardware preflight is unavailable1 required check not read yet or not verified: Processing core / trust.");
    expect(verdictChip().textContent).toBe("Not ready · 1 unknown");
    // Storage that nothing reported is unknown, not "not required".
    expect(row("Storage destination")!.textContent).toMatch(/^Unknown · /);
  });

  it("confirms only on the explicit press, and the confirmation sends nothing to the backend", async () => {
    await render();
    const before = calls().length;
    expect(button("Confirm hardware preflight").disabled).toBe(false);
    expect(host.querySelector("[data-testid=workspace-confirm-why]")).toBeNull();
    await act(async () => button("Confirm hardware preflight").click());
    expect(onConfirm).toHaveBeenCalledTimes(1);
    expect(calls().length).toBe(before);
    await render({ confirmedFor: "sig-now" });
    expect(row("Preflight confirmation")!.textContent).toBe("Confirmed for this setup");
    expect(button("Confirm hardware preflight").disabled).toBe(true);
    await render({ confirmedFor: "sig-before" });
    expect(row("Preflight confirmation")!.textContent).toContain("Setup changed since the confirmation");
  });

  it("a required warning blocks Confirm even when no backend gate fails: gates never unblock a check (#548)", async () => {
    gates = [{ id: "camera.source", status: G.Pass, reason: "", remediation: "" }, { id: "camera.session", status: G.Warn, reason: "w", remediation: "" }, NO_PATH];
    await render({ preflightInput: { ...INPUT, cameraExpected: "Other camera" } });
    expect(gates.some((g) => g.status === G.Fail)).toBe(false);
    expect(button("Confirm hardware preflight").disabled).toBe(true);
    expect(host.querySelector("[data-testid=workspace-confirm-why]")!.textContent).toContain("1 required check with a warning: Camera");
    expect(verdictChip().textContent).toBe("Not ready · 2 warnings");
  });

  it("names the lock when a run is active", async () => {
    await render({ experimentActive: true, confirmLockedReason: "Setup is locked while an experiment is active" });
    expect(button("Confirm hardware preflight").disabled).toBe(true);
    expect(host.querySelector("[data-testid=workspace-confirm-why]")!.textContent).toContain("Setup is locked while an experiment is active");
    expect(verdictChip().textContent).toBe("Readiness paused · a run is active"); // never "Ready to confirm" while Confirm is locked
  });
});

describe("click-through polish (#763)", () => {
  // The gates the walk read with the mock camera idle and no background set.
  const SESSION = { id: "camera.session", status: G.Fail, reason: "camera is idle", remediation: "Start Live View and wait until the camera reports Running" };
  const BACKGROUND = { id: "processing.background", status: G.Warn, reason: "no background image set; detection uses frame thresholds only", remediation: "capture a background (Set Background / calibration)" };

  it("F9: after Confirm readiness, the verdict chips say Confirmed, next to Confirmed for this setup", async () => {
    gates = [SESSION, NO_PATH];
    await render();
    expect(verdictChip().textContent).toBe("Ready to confirm · Start blocked: Camera session");
    await render({ confirmedFor: "sig-now" });
    expect(row("Preflight confirmation")!.textContent).toBe("Confirmed for this setup");
    expect(verdictChip().textContent).toBe("Confirmed · Start blocked: Camera session");
    expect(cardChip().textContent).toBe(verdictChip().textContent);
    expect(host.textContent).not.toContain("Ready to confirm");
    // A changed setup asks again.
    await render({ confirmedFor: "sig-before" });
    expect(verdictChip().textContent).toBe("Ready to confirm · Start blocked: Camera session");
  });

  it("F11: the camera session's fix names Start Camera in the classic view, in the gate row and in the Start-blocked notice", async () => {
    gates = [SESSION, NO_PATH];
    await render();
    const fix = gateRow("camera.session")!.querySelector(".ws-gate-fix")!.textContent!;
    expect(fix).toBe("Fix: in the classic view, press Start Camera and wait until the camera reports Running. The workspace does not start the camera");
    expect(host.querySelector("[data-testid=workspace-start-refused]")!.getAttribute("title")).toContain("Camera session — in the classic view, press Start Camera");
    expect(host.textContent).not.toContain("Start Live View");
  });

  it("F8: with no background set, the background gate says Set Background first", async () => {
    gates = [BACKGROUND, { id: "processing.backgroundCalibration", status: G.Pass, reason: "", remediation: "" }, NO_PATH];
    await render();
    const fix = gateRow("processing.background")!.querySelector(".ws-gate-fix")!.textContent!;
    expect(fix.startsWith("Fix: Set Background first: in the classic view, with the camera running, press Set Background.")).toBe(true);
    expect(fix).toContain("Without a background, Background calibration finds no empty frame");
    expect(gateRow("processing.backgroundCalibration")!.querySelector(".ws-gate-fix")).toBeNull(); // a passing gate has nothing to fix
  });
});

describe("stages only navigate", () => {
  it("selecting a stage calls onStage and sends nothing but the Prepare reads", async () => {
    await render();
    const tabs = [...host.querySelectorAll<HTMLButtonElement>(".mib-stages__tab")];
    expect(tabs.map((t) => t.textContent)).toEqual(["01Prepare", "02Align", "03Experiment", "04Review"]);
    for (const t of tabs) await act(async () => t.click());
    expect(onStage.mock.calls.map(([i]) => i)).toEqual([0, 1, 2, 3]);
    expect(calls().every(isPrepareRead)).toBe(true);
  });

  it("Review says it is not in the workspace yet and offers the way back", async () => {
    await render({ stage: 3 });
    expect(host.textContent).toContain("Not yet in the workspace · Review");
    await act(async () => button("Back to the classic view").click());
    expect(onExit).toHaveBeenCalledTimes(1);
    await advance(PREPARE_READINESS_POLL_MS * 3);
    expect(calls().length).toBe(0); // Review reads nothing
  });

  it("Experiment opens on its Plan (O3): only Prepare's path-less readiness is read, nothing starts", async () => {
    await render({ stage: 2 });
    expect(host.querySelector("[data-testid=workspace-plan]")).not.toBeNull();
    expect(host.textContent).not.toContain("Not yet in the workspace · Experiment");
    expect(host.querySelector("[data-testid=workspace-preview-chip]")!.textContent).toBe("Preview · acts only on a button");
    // Prepare's verdict governs the Plan: the same chip in the header.
    expect(verdictChip().textContent).toBe("Ready · destination checked at Start");
    await advance(PREPARE_READINESS_POLL_MS * 3);
    for (const c of calls()) expect(isPrepareRead(c)).toBe(true);
    expect(planStart).not.toHaveBeenCalled();
    // Live (O4, A6b) has content: choosing it reads the live cells and sends nothing else. Run
    // summary (O5) and Quick look (O6) (A7b, A7c) with no closed run read nothing (no run_quick_look)
    // and send nothing; Quick look shows the plan's row as a queued placeholder.
    await act(async () => button("Live").click());
    expect(host.querySelector("[data-testid=workspace-live]")).not.toBeNull();
    await act(async () => button("Run summary").click());
    expect(host.querySelector("[data-testid=workspace-run-summary] [data-testid=summary-empty]")).not.toBeNull();
    await act(async () => button("Quick look").click());
    expect(host.querySelector("[data-testid=workspace-quick-look] [data-testid=summary-empty]")).not.toBeNull();
    expect(calls().some((c) => c.cmd === "run_quick_look")).toBe(false);
    await act(async () => button("Plan").click());
    expect(host.querySelector("[data-testid=workspace-plan]")).not.toBeNull();
    expect(planStart).not.toHaveBeenCalled();
    expect(onView.mock.calls.map(([v]) => v)).toEqual(["Live", "Run summary", "Quick look", "Plan"]);
    for (const c of calls()) expect(["fetch_experiment_readiness", "calibration_records_list", "fetch_monitoring_cells"]).toContain(c.cmd);
  });

  it("readiness is read only while the Plan is shown: Live, Run summary and Quick look stop the readiness reads, Plan resumes them (#736 review)", async () => {
    await render({ stage: 2 });
    await advance(PREPARE_READINESS_POLL_MS * 2);
    expect(calls().length).toBeGreaterThan(0);
    expect(verdictChip()).not.toBeNull();
    const readiness = () => calls().filter((c) => c.cmd === "fetch_experiment_readiness");
    for (const sub of ["Live", "Run summary", "Quick look"]) {
      await act(async () => button(sub).click());
      const before = readiness().length;
      await advance(PREPARE_READINESS_POLL_MS * 5);
      expect(readiness().length).toBe(before); // no readiness read while Live or the placeholder is shown
      expect(host.querySelector("[data-testid=workspace-verdict]")).toBeNull(); // no verdict without the reads
    }
    const before = readiness().length;
    await act(async () => button("Plan").click());
    await advance(PREPARE_READINESS_POLL_MS * 2);
    expect(readiness().length).toBeGreaterThan(before);
    for (const c of readiness()) expect(c).toEqual({ cmd: "fetch_experiment_readiness", args: { outputPath: "" } });
    // Live read only its cells.
    for (const c of calls()) expect(["fetch_experiment_readiness", "calibration_records_list", "fetch_monitoring_cells"]).toContain(c.cmd);
  });

  it("Align (O2) has content now; opening it sends nothing, switches nothing and writes nothing", async () => {
    await render({ stage: 1 });
    expect(host.querySelector("[data-testid=workspace-align]")).not.toBeNull();
    expect(host.textContent).not.toContain("Not yet in the workspace");
    expect(host.querySelector("[data-testid=workspace-preview-chip]")!.textContent).toBe("Preview · acts only on a button");
    await advance(5000);
    expect(calls().length).toBe(0);
    expect(onSwitchMode).not.toHaveBeenCalled();
    expect(writeRoi).not.toHaveBeenCalled();
    expect(subscribeFrames).toHaveBeenCalledTimes(1); // the one sink for the classic preview's frames
    await act(async () => button("All features · classic view").click());
    expect(onExit).toHaveBeenCalledTimes(1);
  });

  it("Set up camera… / focus… / pumps… leave for the classic Hardware setup page at their section, without a command", async () => {
    await render();
    const before = calls().length;
    await act(async () => button("Set up camera…").click());
    await act(async () => button("Set up focus…").click());
    await act(async () => button("Set up pumps…").click());
    expect(onOpenSetup.mock.calls.map(([s]) => s)).toEqual(["camera", "focus", "fluidics"]);
    expect(calls().length).toBe(before);
  });
});

describe("summary-first Prepare (O1b, approved 2026-10-22)", () => {
  const pass = (id: string): ReadinessGate => ({ id, status: G.Pass, reason: "", remediation: "" });
  // What the mock camera, chosen on purpose and idle, reports (ExperimentCoordinator's words).
  const SESSION: ReadinessGate = { id: "camera.session", status: G.Fail, reason: "camera is idle", remediation: "Start Live View and wait until the camera reports Running" };
  const SIMULATED: ReadinessGate = { id: "camera.source", status: G.Warn, reason: "simulated camera source selected", remediation: "expected for development/tests; not a hardware run" };
  const FALLBACK: ReadinessGate = { id: "camera.source", status: G.Fail, reason: "requested egrabber camera is unavailable; no eGrabber driver on this computer",
    remediation: "select the mock camera explicitly, or install/connect the hardware" };
  const DELIVERY: ReadinessGate = { id: "camera.deliveryMode", status: G.Unavailable, reason: "delivery mode is confirmed only by a running camera", remediation: "" };
  const GEOMETRY: ReadinessGate = { id: "camera.geometry", status: G.Unavailable, reason: "no frame has been received yet", remediation: "wait for the first frame" };
  const LOSS: ReadinessGate = { id: "telemetry.transportLoss", status: G.Unavailable, reason: "no active session", remediation: "" };
  const ROI: ReadinessGate = { id: "processing.roi", status: G.Warn, reason: "no ROI set; full frame is processed", remediation: "set the ROI for this camera window" };
  const BACKGROUND: ReadinessGate = { id: "processing.background", status: G.Warn, reason: "no background image set; detection uses frame thresholds only", remediation: "capture a background (Set Background / calibration)" };
  const CAL_OLD: ReadinessGate = { id: "calibration.record", status: G.Warn, reason: "record 31 h old · policy: warn", remediation: "run the calibration method on this setup, then apply its record" };
  const REST = ["processing.core", "processing.config", "processing.backgroundCalibration", "method.revision", "clock.wall", "lifecycle.experiment",
    "lifecycle.fault", "storage.hdf5", "storage.buffer", "lifecycle.recording"].map(pass);
  const mockIdle = (over: ReadinessGate[] = []): ReadinessGate[] => {
    const base = [SESSION, SIMULATED, DELIVERY, GEOMETRY, LOSS, ROI, BACKGROUND, pass("calibration.record"), ...REST, NO_PATH];
    return base.map((g) => over.find((o) => o.id === g.id) ?? g);
  };
  const NO_STAGE = { valid: true, enabled: true, connected: false, configured: false, zero_set: false, mid_travel_declared: false,
    session_only_zero: false, limits_verified: false, busy: false } as unknown as StageStatus;
  const details = (key: string) => host.querySelector<HTMLDetailsElement>(`details[data-row="${key}"]`)!;
  const summaryOf = (key: string) => details(key).querySelector<HTMLElement>(":scope > summary")!;
  const rowChip = (key: string) => summaryOf(key).querySelector(".mib-chip")!;
  const headline = () => host.querySelector("[data-testid=workspace-headline]")!.textContent;
  /** Shown to the operator: no closed <details> hides it (a closed row still shows its own summary).
   *  textContent counts hidden content too, so this is checked element by element. */
  const visible = (el: Element): boolean => {
    for (let d = el.closest("details"); d; d = d.parentElement?.closest("details") ?? null) {
      if (!d.open && !d.querySelector(":scope > summary")!.contains(el)) return false;
    }
    return true;
  };
  /** Every counted failing gate's text (its value cell with the Fix) is shown outside any closed row. */
  const expectNoFailingGateHidden = (ids: string[]) => {
    for (const id of ids) {
      const cells = [...host.querySelectorAll(`[data-gate-id="${id}"]`)].map((l) => l.closest("dt")!.nextElementSibling!);
      expect(cells.some(visible), `${id} is shown`).toBe(true);
    }
  };

  it("the mock camera chosen on purpose, idle: Hardware ready · simulated camera, every row closed, the Next · Align line shown", async () => {
    gates = mockIdle();
    await render({ zStage: NO_STAGE });
    expect(headline()).toBe("Hardware ready · simulated camera");
    expect(host.querySelector("[data-testid=workspace-detected]")!.textContent).toBe(
      "Mock camera (chosen) · core 2.1.0 trusted · no calibration in effect · optional: focus, pumps and trigger not connected");
    for (const key of ["camera", "focus", "fluidics", "calibration", "method", "destination", "all"]) {
      expect(details(key).open, key).toBe(false);
      expect(details(key).getAttribute("data-open")).toBe("closed");
    }
    const next = host.querySelector("[data-testid=workspace-next-align]")!;
    expect(visible(next)).toBe(true);
    expect(next.textContent).toBe("Next · Alignstart the camera, set ROI and background · Start waits for these 6 gates");
    expect(rowChip("camera").textContent).toBe("Pass · simulated camera");
    expect(rowChip("method").textContent).toBe("Pass · 6 gates");
    expect(rowChip("focus").textContent).toBe("Not connected · optional");
    expect(rowChip("fluidics").textContent).toBe("Not connected · optional");
    expect(summaryOf("focus").textContent).toContain("Autofocus controller, nanopositioner and Z stage · not connected");
    expect(summaryOf("fluidics").textContent).toContain("Sample and sheath pumps · not connected");
    // No configuration on this page: one "Set up …" button per hardware row, no form field.
    expect(host.querySelectorAll("[data-testid=prepare-summary] input, [data-testid=prepare-summary] select, [data-testid=prepare-summary] textarea").length).toBe(0);
    expect(["Set up camera…", "Set up focus…", "Set up pumps…"].map((t) => !!button(t))).toEqual([true, true, true]);
    expect(host.querySelector("[data-testid=workspace-issue-counts]")).toBeNull();
    // No side card on the first page: How Prepare reads readiness is inside All checks.
    expect(visible(host.querySelector("[data-testid=workspace-how]")!)).toBe(false);
    expect(host.querySelector("[data-testid=workspace-how]")!.closest("details")).toBe(details("all"));
  });

  it("Next · Align gates are not counted but shown, in grey, with their note and Fix", async () => {
    gates = mockIdle();
    await render();
    await act(async () => summaryOf("camera").click());
    expect(details("camera").getAttribute("data-open")).toBe("user");
    for (const id of ["camera.session", "camera.deliveryMode", "camera.geometry", "telemetry.transportLoss"]) {
      const cell = gateRow(id, '[data-row="camera"]')!;
      expect(visible(cell)).toBe(true);
      expect(cell.querySelector(".mib-chip")!.getAttribute("data-tone")).toBe("neutral");
      expect(cell.textContent).toContain("Next · Align: not counted in Prepare; Start waits for it");
    }
    expect(gateRow("camera.session", '[data-row="camera"]')!.querySelector(".ws-gate-fix")!.textContent).toContain("press Start Camera");
    const source = gateRow("camera.source", '[data-row="camera"]')!;
    expect(source.querySelector(".mib-chip")!.textContent).toBe("Warn · simulated camera source selected");
    expect(source.querySelector(".mib-chip")!.getAttribute("data-tone")).toBe("neutral");
    expect(source.textContent).toContain("Chosen on purpose");
    // ROI and background are Next · Align too: grey in All checks, never in the Method row's count.
    for (const id of ["processing.roi", "processing.background"]) {
      expect(gateRow(id)!.querySelector(".mib-chip")!.getAttribute("data-tone")).toBe("neutral");
      expect(gateRow(id)!.textContent).toContain("Next · Align: not counted in Prepare");
      expect(gateRow(id, '[data-row="method"]')).toBeUndefined();
    }
    expect(gateRow("processing.background")!.querySelector(".ws-gate-fix")!.textContent).toContain("Set Background first");
  });

  it("Confirm stays enabled while gates would refuse Start, with the one-line refusal note beside it", async () => {
    gates = mockIdle();
    await render();
    const confirm = button("Confirm hardware preflight");
    expect(confirm.disabled).toBe(false);
    const note = host.querySelector("[data-testid=workspace-start-refused]")!;
    expect(note.textContent).toBe("Start is refused until the camera runs in Align.");
    expect(note.parentElement).toBe(confirm.parentElement); // beside it
    expect(visible(note)).toBe(true);
  });

  it("the header chip and the Plan keep prepareVerdict's words", async () => {
    gates = mockIdle();
    await render();
    const expected = prepareVerdict(derivePreflight(INPUT), readinessReply() as never).title;
    expect(expected).toBe("Ready to confirm · Start blocked: Camera session, Frame delivery and 2 more");
    expect(verdictChip().textContent).toBe(expected);
    expect(cardChip().textContent).toBe(expected);
    expect(headline()).not.toBe(expected); // the headline is derived alongside, not instead
    await render({ stage: 2 });
    expect(verdictChip().textContent).toBe(expected);
  });

  it("a counted failing gate: its row opens by itself, cannot be collapsed, and shows its Fix; no failing gate is hidden", async () => {
    gates = mockIdle([FALLBACK, { id: "lifecycle.fault", status: G.Fail, reason: "the previous run faulted", remediation: "acknowledge the fault" }]);
    await render();
    expect(headline()).toBe("Needs attention · 2");
    expect(host.querySelector("[data-testid=workspace-issue-counts]")!.textContent).toBe("2 failed");
    expect(host.querySelector("[data-testid=workspace-detected]")!.textContent).toContain("Mock camera (fallback: eGrabber camera unavailable)");
    for (const key of ["camera", "method"]) {
      expect(details(key).open).toBe(true);
      expect(details(key).getAttribute("data-open")).toBe("issue");
      await act(async () => summaryOf(key).click()); // the operator cannot close it
      expect(details(key).open).toBe(true);
    }
    expect(rowChip("camera").textContent).toBe("Fail · Camera source");
    expect(rowChip("camera").getAttribute("data-tone")).toBe("blocked");
    const fix = gateRow("camera.source", '[data-row="camera"]')!.querySelector(".ws-gate-fix")!;
    expect(fix.textContent).toBe("Fix: select the mock camera explicitly, or install/connect the hardware");
    expect(visible(fix)).toBe(true);
    expectNoFailingGateHidden(["camera.source", "lifecycle.fault"]);
    // The rest of the row is put away behind Show all, and nothing is lost.
    const more = [...details("camera").querySelectorAll<HTMLButtonElement>(".ws-row-more")][0];
    expect(more.textContent).toBe("Camera and Processing core / trust pass · 4 gates wait for Align · Show all");
    expect(gateRow("camera.session", '[data-row="camera"]')).toBeUndefined();
    await act(async () => more.click());
    expect(gateRow("camera.session", '[data-row="camera"]')).toBeDefined();
    expect(host.querySelector("[data-testid=workspace-start-refused]")!.textContent)
      .toBe("Start is refused until Camera source and Previous run fault pass, and the camera runs in Align.");
    // Once the issue is gone, the row returns to closed (the operator never opened it).
    gates = mockIdle();
    await advance(PREPARE_READINESS_POLL_MS);
    expect(details("camera").open).toBe(false);
    expect(headline()).toBe("Hardware ready · simulated camera");
  });

  it("the chosen mock camera's warning never opens the Camera row; a fallback does", async () => {
    gates = mockIdle();
    await render();
    expect(details("camera").open).toBe(false);
    gates = mockIdle([FALLBACK]);
    await advance(PREPARE_READINESS_POLL_MS);
    expect(details("camera").getAttribute("data-open")).toBe("issue");
    expect(headline()).toBe("Needs attention · 1");
  });

  it("storage.output is never counted", async () => {
    gates = [pass("camera.source"), ...REST, NO_PATH];
    await render();
    expect(headline()).toBe("Hardware ready");
    expect(details("destination").open).toBe(false);
    expect(rowChip("destination").textContent).toBe("Unknown · checked at Start");
    expect(summaryOf("destination").textContent).toContain("File chosen and checked at Start · recording file and budget pass");
    expect(gateRow("storage.output", '[data-row="destination"]')!.textContent).toContain("Unavailable · no output path chosen yet");
    expect(host.querySelector("[data-testid=workspace-start-refused]")).toBeNull(); // the destination never refuses here
    expect(host.querySelector("[data-testid=workspace-next-align]")!.textContent).toContain("nothing left");
  });

  it("warnings only: Ready with warnings · N, the row open with its Fix", async () => {
    gates = mockIdle([{ id: "clock.wall", status: G.Warn, reason: "the wall clock is not synchronised", remediation: "enable time sync" }]);
    await render();
    expect(headline()).toBe("Ready with warnings · 1");
    expect(details("method").getAttribute("data-open")).toBe("issue");
    expect(visible(gateRow("clock.wall", '[data-row="method"]')!.querySelector(".ws-gate-fix")!)).toBe(true);
  });

  it("an over-age calibration opens the Calibration row by itself with its Fix", async () => {
    gates = mockIdle([CAL_OLD]);
    await render();
    expect(headline()).toBe("Ready with warnings · 1");
    expect(details("calibration").getAttribute("data-open")).toBe("issue");
    expect(rowChip("calibration").textContent).toBe("Warn · record 31 h old");
    const fix = host.querySelector("[data-testid=calibration-fix]")!;
    expect(fix.textContent).toContain("Fix: run the calibration method");
    expect(visible(fix)).toBe(true);
    expectNoFailingGateHidden(["calibration.record"]);
  });

  it("a proposed record opens the Calibration row by itself, and the operator may close it", async () => {
    gates = mockIdle();
    calList = calListOf([calRecord("cal-p", "proposed", { factor: 0.53 })]);
    await render();
    expect(details("calibration").getAttribute("data-open")).toBe("auto");
    expect(visible(host.querySelector("[data-testid=calibration-proposed]")!)).toBe(true);
    await act(async () => summaryOf("calibration").click());
    expect(details("calibration").open).toBe(false);
    expect(details("calibration").getAttribute("data-open")).toBe("closed");
  });

  it("an applied calibration method opens the Calibration row by itself", async () => {
    gates = mockIdle();
    vi.mocked(invoke).mockImplementation(async (cmd: string) => {
      if (cmd === "fetch_experiment_readiness") return { ...readinessReply(), method_calibration: {
        declared: true, purpose: "calibration", calibration_policy: "none", calibration_max_age_hours: 24,
        calibration_reference: { nominal_diameter_um: 15.2, diameter_tolerance_um: 0.5, nominal_e_kpa: null, max_cv_percent: 5 }, error: null } };
      if (cmd === "calibration_records_list") return calList;
      throw new Error(`unexpected command ${cmd}`);
    });
    await render();
    expect(details("calibration").getAttribute("data-open")).toBe("auto");
    expect(headline()).toBe("Hardware ready · simulated camera"); // it opens, but it is not an issue
  });

  it("gates not read are never a pass: All checks opens with why", async () => {
    vi.mocked(invoke).mockImplementation(() => new Promise(() => {}));
    await render();
    expect(headline()).toBe("Needs attention · 1");
    expect(details("all").getAttribute("data-open")).toBe("issue");
    expect(visible(row("Backend gates")!)).toBe(true);
    expect(row("Backend gates")!.textContent).toBe("Unknown · not read yet");
  });
});
