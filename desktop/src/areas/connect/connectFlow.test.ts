// Connect stage rules (epic #863, B1): the wording is Qt's (ConnectTab.cpp showDiscoveryResults, DeviceInitManager.cpp).
import { describe, expect, it } from "vitest";
import type { CameraDiscovery, CameraSelection, DiscoveredCamera, DiscoveredDevice, DiscoveredFramegrabber, DiscoverySnapshot } from "../../bridge";
import { CAMERA_SELECTION_MODES, DISCOVERY_ERROR_KINDS, DISCOVERY_JOB_STATES } from "../../bridgeContract";
import {
  cameraKey, cameraSourceBlockReason, connectBlockReason, connectedNote, connectStatusLine, discoveryProblemText, discoveryStatusText,
  deviceRows, foundLine, framegrabberKey, isDecidable, NO_PICK, pickedRow, preselectDevices, refreshBlockReason, rowOfSelection,
  selectionSuffix, startupCameraOutcome, SCANNING_TEXT,
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
