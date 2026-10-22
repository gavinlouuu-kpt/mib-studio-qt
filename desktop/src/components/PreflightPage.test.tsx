// @vitest-environment jsdom
// The classic Hardware Preflight first page (#796): the approved O1b summary in the classic shell.
// With the idle mock camera it reads "Hardware ready · simulated camera" with every row closed; a
// counted failing gate opens its row (locked), and no counted failing gate's text sits inside a
// closed row; no form field is rendered; the "Set up …" buttons leave with their section; the
// page reads the gates without a path only while active.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { invoke } from "@tauri-apps/api/core";
import { JSON_TRANSPORT, READINESS_GATE_STATUSES as G } from "../bridgeContract";
import type { ReadinessGate } from "../eventAdapter";
import type { PreflightInput } from "../preflight";
import type { StageStatus } from "../bridge";
import { PREPARE_READINESS_POLL_MS } from "../workspace/prepareModel";
import { isShown } from "../workspace/DisclosureRow";
import { calList as calListOf, calRecord } from "../workspace/calibrationFixtures";
import { PreflightPage, type PreflightPageProps } from "./PreflightPage";

vi.mock("@tauri-apps/api/core", () => ({ invoke: vi.fn() }));

const OFF = { valid: true, connected: false, identity: "" };
const INPUT: PreflightInput = {
  backendReady: true, cameraConfigured: true, cameraRunning: false, cameraIdentity: "Mock camera", cameraExpected: "",
  coreKnown: true, coreValid: true, corePinSatisfied: true, coreVersion: "2.1.0", requiredCoreVersion: "2.1.0",
  autofocus: OFF, samplePump: OFF, sheathPump: OFF, trigger: { valid: true, cameraAttached: false },
  storageKnown: false, storageWritable: false, storageFreeOk: false, storagePath: "",
};
const pass = (id: string): ReadinessGate => ({ id, status: G.Pass, reason: "", remediation: "" });
const NO_PATH: ReadinessGate = { id: "storage.output", status: G.Unavailable, reason: "no output path chosen yet", remediation: "choose an HDF5 destination" };
const SESSION: ReadinessGate = { id: "camera.session", status: G.Fail, reason: "camera is idle", remediation: "Start Live View and wait until the camera reports Running" };
const SIMULATED: ReadinessGate = { id: "camera.source", status: G.Warn, reason: "simulated camera source selected", remediation: "expected for development/tests; not a hardware run" };
const FALLBACK: ReadinessGate = { id: "camera.source", status: G.Fail, reason: "requested egrabber camera is unavailable; no eGrabber driver on this computer", remediation: "select the mock camera explicitly, or install/connect the hardware" };
const CORE_FAIL: ReadinessGate = { id: "processing.core", status: G.Fail, reason: "no core is active", remediation: "activate the pinned core" };
const DELIVERY: ReadinessGate = { id: "camera.deliveryMode", status: G.Unavailable, reason: "delivery mode is confirmed only by a running camera", remediation: "" };
const GEOMETRY: ReadinessGate = { id: "camera.geometry", status: G.Unavailable, reason: "no frame has been received yet", remediation: "wait for the first frame" };
const LOSS: ReadinessGate = { id: "telemetry.transportLoss", status: G.Unavailable, reason: "no active session", remediation: "" };
const ROI: ReadinessGate = { id: "processing.roi", status: G.Warn, reason: "no ROI set; full frame is processed", remediation: "set the ROI for this camera window" };
const BACKGROUND: ReadinessGate = { id: "processing.background", status: G.Warn, reason: "no background image set; detection uses frame thresholds only", remediation: "capture a background (Set Background / calibration)" };
const CAL_OLD: ReadinessGate = { id: "calibration.record", status: G.Warn, reason: "record 31 h old · policy: warn", remediation: "run the calibration method on this setup, then apply its record" };
const REST = ["processing.core", "processing.config", "processing.backgroundCalibration", "method.revision", "clock.wall", "lifecycle.experiment", "lifecycle.fault", "storage.hdf5", "storage.buffer", "lifecycle.recording"].map(pass);
/** What the mock camera, chosen on purpose and idle, reports. */
const mockIdle = (over: ReadinessGate[] = []): ReadinessGate[] =>
  [SESSION, SIMULATED, DELIVERY, GEOMETRY, LOSS, ROI, BACKGROUND, pass("calibration.record"), ...REST, NO_PATH].map((g) => over.find((o) => o.id === g.id) ?? g);
let gates: ReadinessGate[] = mockIdle();
/** No Z stage connected (the desktop without one). */
const NO_STAGE = { valid: true, enabled: false, connected: false, configured: false, zero_set: false } as unknown as StageStatus;
const EMPTY_CAL_LIST = { valid: true, store: { revision: 0, max_versions: 2048, error: null }, applied_record_id: null, in_effect_record_id: null, current_factor: 0.5,
  current_identity: { camera: "mock:Camera 01", roi: "full", chip: "not tracked", objective: "not tracked", known: true }, records: [] };
let calList: unknown = EMPTY_CAL_LIST;
const readinessReply = () => ({ transport_version: JSON_TRANSPORT.version, valid: true, ready: false, generation: "3", gates });

let host: HTMLDivElement, root: Root;
const onConfirm = vi.fn(), onRefreshChecks = vi.fn(), onSetup = vi.fn();
const props = (over: Partial<PreflightPageProps> = {}): PreflightPageProps => ({
  active: true, preflightInput: INPUT, zStage: NO_STAGE, signature: "sig-now", confirmedFor: "", onConfirm, confirmLockedReason: "",
  experimentActive: false, onRefreshChecks, onSetup, controlHeld: true, ...over,
});
const render = (over: Partial<PreflightPageProps> = {}) => act(async () => root.render(<PreflightPage {...props(over)} />));
const advance = (ms: number) => act(async () => { await vi.advanceTimersByTimeAsync(ms); });
const calls = () => vi.mocked(invoke).mock.calls.map(([cmd, args]) => ({ cmd, args }));
const button = (text: string) => [...host.querySelectorAll("button")].find((b) => b.textContent === text)!;
const details = (key: string) => host.querySelector<HTMLDetailsElement>(`details[data-row="${key}"]`)!;
const summaryOf = (key: string) => details(key).querySelector<HTMLElement>(":scope > summary")!;
const rowChip = (key: string) => summaryOf(key).querySelector(".mib-chip")!;
const headline = () => host.querySelector("[data-testid=workspace-headline]")!.textContent;
const gateCells = (id: string) => [...host.querySelectorAll(`[data-gate-id="${id}"]`)].map((l) => l.closest("dt")!.nextElementSibling!);
const ROWS = ["camera", "focus", "fluidics", "calibration", "method", "destination", "all"];

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  vi.useFakeTimers();
  vi.resetAllMocks();
  gates = mockIdle();
  calList = EMPTY_CAL_LIST;
  vi.mocked(invoke).mockImplementation(async (cmd: string) => {
    if (cmd === "fetch_experiment_readiness") return readinessReply();
    if (cmd === "calibration_records_list") return calList;
    throw new Error(`unexpected command ${cmd}`);
  });
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.useRealTimers(); });

describe("the first page with the idle mock camera", () => {
  it("reads Hardware ready · simulated camera, every row closed, the Next · Align line shown, Confirm enabled with the refusal line beside it", async () => {
    await render();
    expect(host.querySelector(".mib-root.classic-preflight")).not.toBeNull();
    expect(headline()).toBe("Hardware ready · simulated camera");
    expect(host.querySelector("[data-testid=workspace-detected]")!.textContent).toBe("Mock camera (chosen) · core 2.1.0 trusted · no calibration in effect · optional: focus, pumps and trigger not connected");
    for (const key of ROWS) {
      expect(details(key).open, key).toBe(false);
      expect(details(key).getAttribute("data-open"), key).toBe("closed");
    }
    expect([...host.querySelectorAll("details[data-row]")].map((d) => d.getAttribute("data-row"))).toEqual(ROWS);
    const next = host.querySelector("[data-testid=workspace-next-align]")!;
    expect(isShown(next)).toBe(true);
    expect(next.textContent).toBe("Next · Alignstart the camera, set ROI and background · Start waits for these 6 gates");
    // The verdict chip keeps prepareVerdict's words (the same as the "Next:" band and the Plan).
    expect(host.querySelector("[data-testid=workspace-prepare-verdict-chip]")!.textContent).toBe("Ready to confirm · Start blocked: Camera session, Frame delivery and 2 more");
    const confirm = button("Confirm hardware preflight");
    expect(confirm.disabled).toBe(false);
    const note = host.querySelector("[data-testid=workspace-start-refused]")!;
    expect(note.textContent).toBe("Start is refused until the camera runs in Align.");
    expect(note.parentElement).toBe(confirm.parentElement);
    expect(host.querySelector("[data-testid=workspace-confirm-why]")).toBeNull();
    expect(host.textContent).toContain("Preflight confirmation");
    expect(host.textContent).toContain("Not confirmed in this browser tab");
    await act(async () => confirm.click());
    expect(onConfirm).toHaveBeenCalledOnce();
  });

  it("renders no form field: hardware configuration lives on the Hardware setup page, one Set up… per hardware row", async () => {
    await render();
    expect(host.querySelectorAll("input, select, textarea").length).toBe(0);
    await act(async () => button("Set up camera…").click());
    await act(async () => button("Set up focus…").click());
    await act(async () => button("Set up pumps…").click());
    expect(onSetup.mock.calls.map(([s]) => s)).toEqual(["camera", "focus", "fluidics"]);
    expect(calls().every((c) => c.cmd === "fetch_experiment_readiness" || c.cmd === "calibration_records_list")).toBe(true); // nothing acted
  });

  it("the Next · Align gates are shown in grey with their note, never counted; storage.output never counts", async () => {
    await render();
    await act(async () => summaryOf("camera").click());
    for (const id of ["camera.session", "camera.deliveryMode", "camera.geometry", "telemetry.transportLoss"]) {
      const cell = gateCells(id).find(isShown)!;
      expect(cell, id).toBeDefined();
      expect(cell.querySelector(".mib-chip")!.getAttribute("data-tone")).toBe("neutral");
      expect(cell.textContent).toContain("Next · Align: not counted in Prepare; Start waits for it");
    }
    expect(rowChip("destination").textContent).toBe("Unknown · checked at Start");
    expect(details("destination").open).toBe(false);
    gates = [pass("camera.source"), ...REST, NO_PATH];
    await advance(PREPARE_READINESS_POLL_MS);
    expect(headline()).toBe("Hardware ready");
    expect(host.querySelector("[data-testid=workspace-start-refused]")).toBeNull();
  });
});

describe("a counted issue", () => {
  it("camera.source fail and processing.core fail: their rows open, locked, with the Fix shown; no counted failing gate's text sits in a closed row", async () => {
    gates = mockIdle([FALLBACK, CORE_FAIL]);
    await render();
    expect(headline()).toBe("Needs attention · 2");
    expect(host.querySelector("[data-testid=workspace-issue-counts]")!.textContent).toBe("2 failed");
    expect(details("camera").open).toBe(true);
    expect(details("camera").getAttribute("data-open")).toBe("issue");
    await act(async () => summaryOf("camera").click());
    expect(details("camera").open).toBe(true); // cannot be collapsed
    expect(rowChip("camera").textContent).toBe("Fail · Camera source and 1 more");
    for (const id of ["camera.source", "processing.core"]) {
      const shown = gateCells(id).filter(isShown);
      expect(shown.length, id).toBeGreaterThan(0);
      expect(shown[0].querySelector(".ws-gate-fix")!.textContent, id).toMatch(/^Fix: /);
    }
    expect(gateCells("camera.source").find(isShown)!.querySelector(".ws-gate-fix")!.textContent).toBe("Fix: select the mock camera explicitly, or install/connect the hardware");
    expect(host.querySelector("[data-testid=workspace-start-refused]")!.textContent).toBe("Start is refused until Camera source and Processing core pass, and the camera runs in Align.");
    for (const key of ["focus", "fluidics", "calibration", "method", "destination", "all"]) expect(details(key).open, key).toBe(false);
    // The issue clears: the row returns to closed.
    gates = mockIdle();
    await advance(PREPARE_READINESS_POLL_MS);
    expect(details("camera").open).toBe(false);
    expect(headline()).toBe("Hardware ready · simulated camera");
  });

  it("a calibration warning auto-expands the Calibration row with its Fix and Re-apply", async () => {
    const applied = calRecord("cal-1", "applied", { factor: 0.52 });
    calList = calListOf([applied], { applied_record_id: "cal-1", in_effect_record_id: null, current_factor: 0.5 });
    gates = mockIdle([{ ...CAL_OLD, status: G.Fail, reason: "applied record cal-1 is not in effect · policy: required", remediation: "re-apply record cal-1" }]);
    await render();
    expect(details("calibration").getAttribute("data-open")).toBe("issue");
    expect(isShown(host.querySelector("[data-testid=calibration-fix]")!)).toBe(true);
    const reapply = host.querySelector<HTMLButtonElement>("[data-testid=calibration-reapply-cal-1]")!;
    expect(reapply).not.toBeNull();
    expect(isShown(reapply)).toBe(true);
    expect(reapply.disabled).toBe(false);
    expect(headline()).toBe("Needs attention · 1");
  });

  it("gates not read are never a pass", async () => {
    vi.mocked(invoke).mockImplementation(() => new Promise(() => {}));
    await render();
    expect(headline()).toBe("Needs attention · 1");
    expect(details("all").getAttribute("data-open")).toBe("issue");
  });
});

describe("reads", () => {
  it("polls the gates without a path, and the records, every 2 s while active; nothing while inactive (the workspace open, or a run)", async () => {
    await render();
    await advance(PREPARE_READINESS_POLL_MS * 3);
    const log = calls();
    expect(log.filter((c) => c.cmd === "fetch_experiment_readiness").length).toBe(4);
    for (const c of log) expect(["fetch_experiment_readiness", "calibration_records_list"]).toContain(c.cmd);
    for (const c of log.filter((c) => c.cmd === "fetch_experiment_readiness")) expect(c.args).toEqual({ outputPath: "" });
    await render({ active: false });
    const before = calls().length;
    await advance(PREPARE_READINESS_POLL_MS * 3);
    expect(calls().length).toBe(before);
    await render({ active: false, experimentActive: true, confirmLockedReason: "Setup is locked while an experiment is active" });
    await advance(PREPARE_READINESS_POLL_MS * 3);
    expect(calls().length).toBe(before);
    expect(headline()).toBe("Readiness paused · a run is active");
    expect(button("Confirm hardware preflight").disabled).toBe(true);
  });

  it("Refresh re-reads the checklist devices and the gates (reads only)", async () => {
    await render();
    await act(async () => summaryOf("all").click());
    const before = calls().length;
    await act(async () => button("Refresh").click());
    await advance(0);
    expect(onRefreshChecks).toHaveBeenCalledOnce();
    expect(calls().length).toBeGreaterThan(before);
    expect(calls().every((c) => c.cmd === "fetch_experiment_readiness" || c.cmd === "calibration_records_list")).toBe(true);
  });
});
