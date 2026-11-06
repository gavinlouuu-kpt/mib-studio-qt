// Connect stage rules (epic #863, B1): the wording is Qt's (ConnectTab.cpp showDiscoveryResults, DeviceInitManager.cpp).
import { describe, expect, it, vi } from "vitest";
import type { CameraDiscovery, CameraSelection, CaptureLifecycle, DiscoveredCamera, DiscoveredDevice, DiscoveredFramegrabber, DiscoverySnapshot } from "../../bridge";
import { CAMERA_SELECTION_MODES, DISCOVERY_ERROR_KINDS, DISCOVERY_JOB_STATES, EXPERIMENT_STATES } from "../../bridgeContract";
import {
  cameraKey, cameraSourceBlockReason, connectBlockReason, connectedNote, connectStatusLine, discoveryProblemText, discoveryStatusText,
  deviceRows, foundLine, framegrabberKey, isDecidable, NO_PICK, pickedRow, preselectDevices, refreshBlockReason, rowOfSelection,
  selectionSuffix, startupCameraOutcome, SCANNING_TEXT,
  cameraPhase, defaultMockFolder, loadMockSettings, MOCK_STORAGE_KEY, mockFpsProblem, parseMockFps, saveMockSettings, spaceTogglesCamera,
  holdCameraCommand, spaceCameraAction, startCameraBlockReason, STOP_WHILE_RECORDING, stopCameraBlockReason, type CameraCommand,
} from "./connectFlow";

const cam = (p: Partial<DiscoveredCamera>): DiscoveredCamera => ({
  camera_type: 0, camera_index: -1, interface_index: 0, device_index: 0, interface_id: "", device_id: "", model_name: "", firmware_version: "",
  label: "EGrabber 0", ...p,
});
const fg = (p: Partial<DiscoveredFramegrabber>): DiscoveredFramegrabber => ({
  interface_index: 0, device_index: 0, stream_index: 0, interface_id: "", device_id: "", stream_id: "", model_name: "", label: "Coaxlink 0", ...p,
});
const discovery = (p: Partial<CameraDiscovery> = {}): CameraDiscovery => ({
  valid: true, complete: true, job_id: "1", cameras: [], framegrabbers: [], state: DISCOVERY_JOB_STATES.Completed, errors: [], ...p,
});
const mockCam = cam({ camera_type: 2, label: "Mock camera (folder frame stream)" });
const err = (provider_id: string, kind: number) => ({ provider_id, kind, message: "", endpoint: "" });
const selection = (p: Partial<CameraSelection>): CameraSelection => ({
  valid: true, mode: 0, interface_index: -1, device_index: -1, label: "", mindvision_index: -1, mindvision_config_path: "", camera_script_path: "",
  mock_frame_dir: "", mock_interval_ms: 33, mock_loop: true, configured: false, running: false, ...p,
});

describe("the status line (CONNECT-007)", () => {
  it("counts what was found", () => {
    expect(foundLine(discovery({ framegrabbers: [fg({})], cameras: [cam({}), cam({ camera_type: 1, label: "MV" }), mockCam] })))
      .toBe("Found 1 framegrabber(s), 1 eGrabber camera(s), 1 MindVision camera(s)");
  });
  it("a cancelled scan is never '0 cameras found'", () => {
    expect(discoveryStatusText(discovery({ state: DISCOVERY_JOB_STATES.Cancelled, valid: false, complete: false }))).toBe("Camera scan cancelled.");
  });
  it("names the first coverage gap of an incomplete scan, skipping a compiled-out SDK", () => {
    const d = discovery({ complete: false, errors: [err("mindvision", DISCOVERY_ERROR_KINDS.MissingSdk), err("egrabber", DISCOVERY_ERROR_KINDS.Busy), err("x", DISCOVERY_ERROR_KINDS.Timeout)] });
    expect(discoveryStatusText(d)).toBe("Found 0 framegrabber(s), 0 eGrabber camera(s), 0 MindVision camera(s) (incomplete: egrabber Busy)");
    expect(discoveryStatusText(discovery({ complete: false, errors: [err("mindvision", DISCOVERY_ERROR_KINDS.MissingSdk)] })))
      .toBe("Found 0 framegrabber(s), 0 eGrabber camera(s), 0 MindVision camera(s)");
  });
  it("a failed scan says so when no gap is named", () => {
    expect(discoveryStatusText(discovery({ state: DISCOVERY_JOB_STATES.Failed, valid: false, complete: false }))).toMatch(/\(scan failed\)$/);
  });
  it("a complete scan is just the found line", () => {
    expect(discoveryStatusText(discovery({ cameras: [cam({})] }))).toBe("Found 0 framegrabber(s), 1 eGrabber camera(s), 0 MindVision camera(s)");
  });
  it("a refused or timed-out scan is 'Camera discovery incomplete: <reason>' (CONNECT-005/026)", () => {
    expect(discoveryProblemText("capture is running")).toBe("Camera discovery incomplete: capture is running");
  });
  it("shows one line in priority order: scanning, the last action, the last scan with the selection", () => {
    const base = { scanning: false, actionNote: "", scanNote: "", discovery: discovery(), selection: selection({}) };
    expect(connectStatusLine({ ...base, scanning: true, actionNote: "x" })).toBe(SCANNING_TEXT);
    expect(connectStatusLine({ ...base, actionNote: connectedNote("Cam") })).toBe("Connected to Cam (not capturing)");
    // Q1: Qt's label goes stale once the camera starts; here it follows the live state
    expect(connectStatusLine({ ...base, running: true, actionNote: connectedNote("Cam") })).toBe("Connected to Cam (capturing)");
    expect(connectStatusLine({ ...base, running: true, actionNote: "Mock camera configured (not capturing)" })).toBe("Mock camera configured (capturing)");
    expect(connectStatusLine({ ...base, running: true, actionNote: "Delivery mode set to Latest Frame." })).toBe("Delivery mode set to Latest Frame.");
    expect(connectStatusLine({ ...base, scanNote: "Camera scan cancelled." })).toBe("Camera scan cancelled. · no camera selected");
    expect(connectStatusLine(base)).toBe("Found 0 framegrabber(s), 0 eGrabber camera(s), 0 MindVision camera(s) · no camera selected");
    expect(selectionSuffix(selection({ configured: true, mode: CAMERA_SELECTION_MODES.Mock, mock_frame_dir: "/m" }))).toBe(" · selected: mock (/m)");
    expect(selectionSuffix(selection({ configured: true, mode: CAMERA_SELECTION_MODES.Hardware, label: "EGrabber 0" }))).toBe(" · selected: EGrabber 0");
  });
});

describe("selection (CONNECT-002/003)", () => {
  const d = discovery({
    cameras: [cam({ label: "A", device_index: 0 }), cam({ label: "B", device_index: 1 }), cam({ camera_type: 1, camera_index: 3, label: "MV3" }), mockCam],
    framegrabbers: [fg({ label: "FG0" }), fg({ label: "FG1", device_index: 1 })],
  });
  it("lists the rows of each sub-tab, framegrabbers included", () => {
    expect(deviceRows(d, "cameras").map((r) => r.label)).toEqual(["A", "B", mockCam.label]);
    expect(deviceRows(d, "mindvision").map((r) => r.label)).toEqual(["MV3"]);
    expect(deviceRows(d, "framegrabbers").map((r) => r.label)).toEqual(["FG0", "FG1"]);
    expect(deviceRows(null, "cameras")).toEqual([]);
  });
  it("preselects row 0 of every list after a scan, never the synthetic mock source", () => {
    const p = preselectDevices(d);
    expect(p.cameras).toBe(cameraKey(d.cameras[0])); expect(p.mindvision).toBe(cameraKey(d.cameras[2])); expect(p.framegrabbers).toBe(framegrabberKey(d.framegrabbers[0]));
    expect(preselectDevices(discovery({ cameras: [mockCam] })).cameras).toBeNull();
    expect(preselectDevices(null)).toEqual(NO_PICK);
  });
  it("keeps a row the operator picked while the next scan still lists it, else falls back to row 0", () => {
    const second = cameraKey(d.cameras[1]);
    expect(preselectDevices(d, { ...NO_PICK, cameras: second }).cameras).toBe(second);
    expect(preselectDevices(discovery({ cameras: [d.cameras[0]] }), { ...NO_PICK, cameras: second }).cameras).toBe(cameraKey(d.cameras[0]));
  });
  it("Connect acts on the shown sub-tab's pick, not a row hidden on another sub-tab", () => {
    const picked = { cameras: cameraKey(d.cameras[1]), mindvision: null, framegrabbers: framegrabberKey(d.framegrabbers[1]) };
    expect(pickedRow(d, "cameras", picked)?.label).toBe("B");
    expect(pickedRow(d, "mindvision", picked)).toBeNull();
    const row = pickedRow(d, "framegrabbers", picked);
    expect(row?.tab === "framegrabbers" && row.framegrabber.device_index).toBe(1);
    expect(pickedRow(d, "cameras", { ...picked, cameras: "0:9:9:9" })).toBeNull();
  });
  it("finds the row the backend's selection points at (the auto-selected camera)", () => {
    expect(rowOfSelection(d, selection({ configured: true, mode: CAMERA_SELECTION_MODES.Hardware, interface_index: 0, device_index: 1 }))?.label).toBe("B");
    expect(rowOfSelection(d, selection({ configured: true, mode: CAMERA_SELECTION_MODES.MindVision, mindvision_index: 3 }))).toMatchObject({ tab: "mindvision", label: "MV3" });
    expect(rowOfSelection(d, selection({ configured: true, mode: CAMERA_SELECTION_MODES.Mock }))?.label).toBe(mockCam.label);
    expect(rowOfSelection(d, selection({ configured: false }))).toBeNull();
    expect(rowOfSelection(d, selection({ configured: true, mode: CAMERA_SELECTION_MODES.Hardware, device_index: 7 }))).toBeNull();
  });
});

describe("why a button is blocked (CONNECT-009/013)", () => {
  const ok = { ready: true, running: false, expActive: false, scriptBusy: false };
  it("names the reason in Qt's words", () => {
    expect(cameraSourceBlockReason(ok)).toBeUndefined();
    expect(cameraSourceBlockReason({ ...ok, running: true })).toBe("Cannot change camera connection while camera is running. Please stop the camera first.");
    expect(cameraSourceBlockReason({ ...ok, expActive: true })).toMatch(/while an experiment is active/);
    expect(cameraSourceBlockReason({ ...ok, scriptBusy: true })).toBe("Camera setup is in progress");
    expect(cameraSourceBlockReason({ ...ok, ready: false, running: true })).toBe("Backend is not initialized");
  });
  it("Connect also needs a row of the shown sub-tab", () => {
    const d = discovery({ cameras: [cam({})] });
    const row = pickedRow(d, "cameras", preselectDevices(d));
    expect(connectBlockReason(ok, "cameras", row)).toBeUndefined();
    expect(connectBlockReason(ok, "mindvision", null)).toBe("Please select a MindVision camera from the list.");
    expect(connectBlockReason(ok, "framegrabbers", null)).toBe("Please select a framegrabber from the list.");
    expect(connectBlockReason({ ...ok, running: true }, "cameras", row)).toMatch(/camera is running/);
  });
  it("Refresh is blocked only while scanning; a refusal while capturing reaches the status line instead", () => {
    expect(refreshBlockReason(true, false)).toBeUndefined();
    expect(refreshBlockReason(true, true)).toBe(SCANNING_TEXT);
    expect(refreshBlockReason(false, false)).toBe("Backend is not initialized");
  });
});

describe("the startup scan's outcome (CONNECT-025/026)", () => {
  const dev = (p: Partial<DiscoveredDevice>): DiscoveredDevice => ({
    kind: 0, provider_id: "p", display_name: "", system_path: "", persistent_id: "", sdk_index: 0, interface_index: 0, device_index: 0, stream_index: -1,
    bus_address: -1, stable_identity: "", identity_strength: 1, identification: 0, claimed_by: [], capabilities: [], synthetic: false, camera_type: 0,
    interface_id: "", device_id: "", stream_id: "", model_name: "", firmware_version: "", label: "", ...p,
  });
  const snap = (p: Partial<DiscoverySnapshot>): DiscoverySnapshot => ({
    valid: true, job_id: "5", generation: "5", state: DISCOVERY_JOB_STATES.Completed, complete: true, overflow: false, attempt: 1, max_attempts: 1,
    kinds: [0, 1], candidates: [], errors: [], providers_run: [], origin: "startup-camera", ...p,
  });
  it("no identified camera is 'No cameras found.'", () => {
    expect(startupCameraOutcome(snap({}))).toEqual({ kind: "none", text: "No cameras found." });
    // a framegrabber, a synthetic mock and an unidentified camera are not cameras to select
    expect(startupCameraOutcome(snap({ candidates: [dev({ kind: 1 }), dev({ synthetic: true }), dev({ identification: 1 })] })).kind).toBe("none");
  });
  it("one identified camera is selected by the backend; several need the operator", () => {
    expect(startupCameraOutcome(snap({ candidates: [dev({})] })).kind).toBe("one");
    expect(startupCameraOutcome(snap({ candidates: [dev({}), dev({ device_index: 1 })] }))).toEqual({ kind: "several", text: "Multiple cameras found; select one and click Connect." });
  });
  it("an incomplete, cancelled or overflowing scan is never 'no cameras'", () => {
    expect(startupCameraOutcome(snap({ complete: false, errors: [err("egrabber", DISCOVERY_ERROR_KINDS.Busy)] })))
      .toEqual({ kind: "incomplete", text: "Camera discovery incomplete: egrabber Busy" });
    expect(startupCameraOutcome(snap({ state: DISCOVERY_JOB_STATES.Cancelled, complete: false })))
      .toEqual({ kind: "incomplete", text: "Camera discovery incomplete: camera discovery Cancelled (incomplete coverage)" });
    expect(startupCameraOutcome(snap({ overflow: true })).kind).toBe("incomplete");
    expect(startupCameraOutcome(snap({ complete: false })).kind).toBe("incomplete");
  });
  it("a compiled-out SDK does not make a scan undecidable (the backend's rule)", () => {
    const missing = snap({ complete: false, errors: [err("mindvision", DISCOVERY_ERROR_KINDS.MissingSdk), err("x", DISCOVERY_ERROR_KINDS.Unsupported)] });
    expect(isDecidable(missing)).toBe(true);
    expect(startupCameraOutcome(missing).kind).toBe("none");
    expect(isDecidable(snap({ complete: false, errors: [err("mindvision", DISCOVERY_ERROR_KINDS.MissingSdk), err("egrabber", DISCOVERY_ERROR_KINDS.Timeout)] }))).toBe(false);
  });
});

// ---- Start / Stop Camera (B2) ----

const lifecycle = (p: Partial<CaptureLifecycle> = {}): CaptureLifecycle => ({
  valid: true, state: "idle", generation: "3", camera_ready: false, failure: "none", message: "", failure_generation: "0", ...p,
});

describe("the camera's phase text (CONNECT-034, Qt CameraController::project)", () => {
  it("names each lifecycle state", () => {
    expect(cameraPhase(lifecycle(), true, false)).toMatchObject({ phase: "idle", text: "Camera idle" });
    expect(cameraPhase(lifecycle({ state: "starting" }), true, false)).toMatchObject({ phase: "starting", text: "Camera starting\u2026" });
    expect(cameraPhase(lifecycle({ state: "running", camera_ready: true }), true, true)).toMatchObject({ phase: "running", text: "Camera running" });
    expect(cameraPhase(lifecycle({ state: "stopping" }), true, false)).toMatchObject({ phase: "stopping", text: "Camera stopping\u2026" });
  });
  it("a faulted worker, or an idle one whose failure belongs to this session, is 'Camera failed' with the reason", () => {
    expect(cameraPhase(lifecycle({ state: "faulted", failure: "streamEnded", message: "camera ended its stream" }), true, false))
      .toEqual({ phase: "failed", text: "Camera failed", failureMessage: "camera ended its stream" });
    expect(cameraPhase(lifecycle({ failure: "cameraStartFailed", message: "open failed", failure_generation: "3" }), true, false).phase).toBe("failed");
    // a failure kept from an earlier session is not this one's
    expect(cameraPhase(lifecycle({ failure: "cameraStartFailed", message: "open failed", failure_generation: "2" }), true, false)).toMatchObject({ phase: "idle", failureMessage: "" });
    expect(cameraPhase(lifecycle({ state: "faulted", failure: "exception", message: "" }), true, false).failureMessage).toBe("exception");
  });
  it("nothing active and no camera chosen is 'Camera not configured'", () => {
    expect(cameraPhase(lifecycle(), false, false)).toMatchObject({ phase: "notConfigured", text: "Camera not configured" });
    expect(cameraPhase(lifecycle({ state: "running" }), false, true).phase).toBe("running");
  });
  it("without a lifecycle reading the running flag decides", () => {
    expect(cameraPhase(null, true, true).phase).toBe("running");
    expect(cameraPhase(null, true, false).phase).toBe("idle");
    expect(cameraPhase({ ...lifecycle(), valid: false }, false, false).phase).toBe("notConfigured");
  });
});

describe("one camera command at a time and why a command is blocked (CONNECT-033/038)", () => {
  const go = { updateBusy: false, scriptBusy: false, ready: true, configured: true, running: false, phase: "idle" as const, commandInFlight: false };
  it("Start names its reason in Qt's words", () => {
    expect(startCameraBlockReason(go)).toBeUndefined();
    expect(startCameraBlockReason({ ...go, configured: false })).toBe("No camera is configured. Please connect to a camera or configure a mock camera first.");
    expect(startCameraBlockReason({ ...go, running: true })).toBe("Camera is already running");
    expect(startCameraBlockReason({ ...go, commandInFlight: true })).toBe("A camera command is already in progress.");
    expect(startCameraBlockReason({ ...go, phase: "stopping" })).toBe("The camera is still stopping; try again in a moment.");
    expect(startCameraBlockReason({ ...go, phase: "starting" })).toBe("Camera is starting");
    expect(startCameraBlockReason({ ...go, updateBusy: true })).toBe("Application update is pending");
    expect(startCameraBlockReason({ ...go, scriptBusy: true, configured: false })).toBe("Camera setup is in progress");
    expect(startCameraBlockReason({ ...go, ready: false })).toBe("Backend is not initialized");
  });
  it("Stop is blocked during an experiment and while its data is being saved, with the reason", () => {
    const stop = { running: true, expState: EXPERIMENT_STATES.Idle, commandInFlight: false };
    expect(stopCameraBlockReason(stop)).toBeUndefined();
    expect(stopCameraBlockReason({ ...stop, running: false })).toBe("Camera is not running");
    expect(stopCameraBlockReason({ ...stop, expState: EXPERIMENT_STATES.Failed })).toBeUndefined();
    // A Failed run still saving after a fatal save error (status.flushing; Qt flushInProgress_) holds the camera too.
    expect(stopCameraBlockReason({ ...stop, expState: EXPERIMENT_STATES.Failed, flushing: true })).toBe("Cannot stop camera while experiment data is being saved. Wait for the save to finish.");
    expect(stopCameraBlockReason({ ...stop, expState: EXPERIMENT_STATES.Failed, flushing: false })).toBeUndefined();
    expect(stopCameraBlockReason({ ...stop, expState: EXPERIMENT_STATES.Active })).toBe("Cannot stop camera while experiment is active. Please stop the experiment first.");
    expect(stopCameraBlockReason({ ...stop, expState: EXPERIMENT_STATES.Stopping })).toBe("Cannot stop camera while experiment data is being saved. Wait for the save to finish.");
    expect(stopCameraBlockReason({ ...stop, expState: EXPERIMENT_STATES.Starting })).toBe("Cannot stop camera while an experiment is starting.");
    expect(stopCameraBlockReason({ ...stop, commandInFlight: true })).toBe("A camera command is already in progress.");
  });
});

describe("the space bar (CONNECT-041)", () => {
  const ctx = { tab: "experiment", expTab: "preview", expActive: false, ready: true };
  const space = (over: Record<string, unknown> = {}) => ({ key: " ", code: "Space", target: { tagName: "BODY" }, ...over });
  it("toggles the camera on the Experiment Preview (Qt's PlaybackPanel) when nothing else owns the key", () => {
    expect(spaceTogglesCamera(space(), ctx)).toBe(true);
    expect(spaceTogglesCamera(space({ target: null }), ctx)).toBe(true);
    expect(spaceTogglesCamera(space({ key: "Spacebar", code: "Space" }), ctx)).toBe(true);
    expect(spaceTogglesCamera(space({ target: { tagName: "DIV", role: "presentation" } }), ctx)).toBe(true);
  });
  it("never during an experiment, on other stages or Experiment sub-tabs, or before the backend is ready", () => {
    expect(spaceTogglesCamera(space(), { ...ctx, tab: "overview" })).toBe(false);
    expect(spaceTogglesCamera(space(), { ...ctx, expTab: "monitoring" })).toBe(false);
    expect(spaceTogglesCamera(space(), { ...ctx, expActive: true })).toBe(false);
    expect(spaceTogglesCamera(space(), { ...ctx, tab: "connect" })).toBe(false);
    expect(spaceTogglesCamera(space(), { ...ctx, tab: "review" })).toBe(false);
    expect(spaceTogglesCamera(space(), { ...ctx, ready: false })).toBe(false);
  });
  it("not when the focus is in a text field, a button, a list or a dialog", () => {
    for (const tagName of ["INPUT", "TEXTAREA", "SELECT", "BUTTON", "A", "SUMMARY"]) expect(spaceTogglesCamera(space({ target: { tagName } }), ctx), tagName).toBe(false);
    expect(spaceTogglesCamera(space({ target: { tagName: "DIV", isContentEditable: true } }), ctx)).toBe(false);
    expect(spaceTogglesCamera(space({ target: { tagName: "DIV", role: "checkbox" } }), ctx)).toBe(false);
    expect(spaceTogglesCamera(space({ target: { tagName: "DIV", role: "listbox" } }), ctx)).toBe(false);
    expect(spaceTogglesCamera(space({ target: { tagName: "DIV", inDialog: true } }), ctx)).toBe(false);
  });
  it("only a plain, first press of the space bar", () => {
    expect(spaceTogglesCamera(space({ key: "a", code: "KeyA" }), ctx)).toBe(false);
    for (const mod of ["ctrlKey", "altKey", "metaKey", "shiftKey", "repeat", "defaultPrevented"]) expect(spaceTogglesCamera(space({ [mod]: true }), ctx), mod).toBe(false);
  });
});

describe("what the space bar does (CONNECT-041, decision CONNECT-039)", () => {
  const idle = { running: false, recording: false, commandInFlight: false };
  it("starts a stopped camera and stops a running one", () => {
    expect(spaceCameraAction(idle)).toEqual({ kind: "start" });
    expect(spaceCameraAction({ ...idle, running: true })).toEqual({ kind: "stop" });
  });
  it("says why Start is refused, in Qt's words", () => {
    expect(spaceCameraAction({ ...idle, startReason: "The camera is still stopping; try again in a moment." }))
      .toEqual({ kind: "refuse", message: "The camera is still stopping; try again in a moment." });
  });
  it("never ends a raw recording: Stop is refused with Qt's text", () => {
    expect(spaceCameraAction({ ...idle, running: true, recording: true })).toEqual({ kind: "refuse", message: STOP_WHILE_RECORDING });
    expect(STOP_WHILE_RECORDING).toBe("Cannot stop camera while frame recording is active. Stop recording first.");
  });
  it("a press while a command is in flight is ignored, silently", () => {
    expect(spaceCameraAction({ ...idle, commandInFlight: true, startReason: "x" })).toEqual({ kind: "none" });
    expect(spaceCameraAction({ ...idle, commandInFlight: true, running: true })).toEqual({ kind: "none" });
  });
});

describe("the mock camera dialog's defaults (CONNECT-014/016)", () => {
  it("the frame rate is fps from 1 to 10000, the rig's 5000 by default", () => {
    expect(parseMockFps("5000")).toBe(5000);
    expect(parseMockFps(" 1 ")).toBe(1);
    expect(parseMockFps("10000")).toBe(10000);
    expect(parseMockFps("33.5")).toBe(33.5);
    for (const bad of ["", "  ", "0", "0.5", "10001", "-3", "abc", "NaN", "Infinity"]) expect(parseMockFps(bad), bad).toBeNull();
    expect(mockFpsProblem("5000")).toBeUndefined();
    expect(mockFpsProblem("0")).toBe("Frame rate must be 1 to 10000 fps");
  });
  it("the folder: the one used last time, else the backend's mock folder, else Documents; trimmed", () => {
    expect(defaultMockFolder(" /last ", "/backend", "/docs")).toBe("/last");
    expect(defaultMockFolder("", " /backend ", "/docs")).toBe("/backend");
    expect(defaultMockFolder("  ", "", " /docs ")).toBe("/docs");
    expect(defaultMockFolder("", "", "")).toBe("");
  });
  it("is remembered across sessions, trimmed, and a bad stored value is ignored", () => {
    const store = new Map<string, string>();
    const storage = { getItem: (k: string) => store.get(k) ?? null, setItem: (k: string, v: string) => { store.set(k, v); } };
    expect(loadMockSettings(storage)).toEqual({});
    expect(saveMockSettings(storage, { dir: "  /frames ", fps: " 2500 ", loop: false })).toBe(true);
    expect(loadMockSettings(storage)).toEqual({ dir: "/frames", fps: "2500", loop: false });
    store.set(MOCK_STORAGE_KEY, JSON.stringify({ dir: 5, fps: "99999", loop: "yes" }));
    expect(loadMockSettings(storage)).toEqual({});
    store.set(MOCK_STORAGE_KEY, "{broken");
    expect(loadMockSettings(storage)).toEqual({});
    expect(saveMockSettings({ setItem: () => { throw new Error("quota"); } }, { dir: "/x", fps: "1", loop: true })).toBe(false);
  });
});

// #863 integration: a stage change (A) stops, reconfigures and restarts the camera, and must not run beside a Start / Stop Camera
// click (B's one-command guard) or leave the Start button free in between.
describe("holdCameraCommand", () => {
  const guard = () => {
    const ref: { current: CameraCommand | null } = { current: null };
    const seen: Array<CameraCommand | null> = [];
    return { ref, seen, set: (c: CameraCommand | null) => { seen.push(c); } };
  };
  it("holds the guard while the work runs and releases it after, also when the work throws", async () => {
    const g = guard();
    let during: CameraCommand | null = null;
    await holdCameraCommand(g, async () => { during = g.ref.current; });
    expect(during).toBe("stage");
    expect(g.ref.current).toBeNull();
    expect(g.seen).toEqual(["stage", null]);
    await expect(holdCameraCommand(g, async () => { throw new Error("boom"); })).rejects.toThrow("boom");
    expect(g.ref.current).toBeNull();
    expect(g.seen).toEqual(["stage", null, "stage", null]);
  });
  it("returns the work's result", async () => {
    expect(await holdCameraCommand(guard(), async () => 42)).toBe(42);
  });
  it("makes the Start and Stop buttons say a command is in progress while it runs", async () => {
    const g = guard();
    let start: string | undefined, stop: string | undefined;
    await holdCameraCommand(g, async () => {
      const inFlight = g.ref.current !== null;
      start = startCameraBlockReason({ updateBusy: false, scriptBusy: false, ready: true, configured: true, running: false, phase: "idle", commandInFlight: inFlight });
      stop = stopCameraBlockReason({ running: true, expState: EXPERIMENT_STATES.Idle, commandInFlight: inFlight });
    });
    expect(start).toBe("A camera command is already in progress.");
    expect(stop).toBe("A camera command is already in progress.");
  });
  it("waits for a Start or Stop that is in flight, then takes the guard", async () => {
    vi.useFakeTimers();
    try {
      const g = guard();
      g.ref.current = "start";
      const order: string[] = [];
      const held = holdCameraCommand(g, async () => { order.push(`work:${g.ref.current}`); });
      await vi.advanceTimersByTimeAsync(100);
      expect(order).toEqual([]); // still waiting
      g.ref.current = null; // the Start finished
      await vi.advanceTimersByTimeAsync(100);
      await held;
      expect(order).toEqual(["work:stage"]);
      expect(g.ref.current).toBeNull();
    } finally {
      vi.useRealTimers();
    }
  });
  it("runs without the guard, and leaves the other command's guard alone, when that command never ends", async () => {
    vi.useFakeTimers();
    try {
      const g = guard();
      g.ref.current = "stop";
      const order: string[] = [];
      const held = holdCameraCommand(g, async () => { order.push(`work:${g.ref.current}`); }, 200);
      await vi.advanceTimersByTimeAsync(300);
      await held;
      expect(order).toEqual(["work:stop"]);
      expect(g.ref.current).toBe("stop");
      expect(g.seen).toEqual([]);
    } finally {
      vi.useRealTimers();
    }
  });
});
