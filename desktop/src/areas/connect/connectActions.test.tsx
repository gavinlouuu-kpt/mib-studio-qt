// @vitest-environment jsdom
// Connect stage actions (epic #863, B1): a scan that is refused says so, Refresh joins the running scan, Connect acts
// on the shown sub-tab (framegrabbers included), a vanished device is said, a refused command raises a warning, and
// the stage moves to Overview after Connect / Configure Mock.
import { act, useState } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { CameraDiscovery } from "../../bridge";
import { CAMERA_SELECTION_MODES, DISCOVERY_JOB_STATES } from "../../bridgeContract";
import { DiscoveryRefusedError } from "../../discovery";
import { bridge } from "../../bridge";
import { cameraKey, framegrabberKey } from "./connectFlow";
import { useCameraConnection, useConnectState } from "./useConnect";

vi.mock("../../bridge", () => ({
  bridge: {
    discoverCameras: vi.fn(), fetchCameraSelection: vi.fn(), selectHardwareCamera: vi.fn(), selectMindVisionCamera: vi.fn(),
    configureMock: vi.fn(), configureMockFps: vi.fn(), setFrameDeliveryMode: vi.fn(), fetchFrameDeliveryMode: vi.fn(),
    startCapture: vi.fn(), stopCapture: vi.fn(), stopRecording: vi.fn(), appPaths: vi.fn(),
  },
}));

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let host: HTMLDivElement, root: Root;
beforeEach(() => { vi.clearAllMocks(); runningNow = false; recordingNow = false; expStateNow = 0; host = document.createElement("div"); document.body.append(host); root = createRoot(host); });
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

type Captured = { connect: ReturnType<typeof useConnectState>; model: ReturnType<typeof useCameraConnection>; tab: string; log: string[] };
let captured: Captured;
const log: string[] = [];
let runningNow = false, recordingNow = false, expStateNow = 0;
const setRecordingMock = vi.fn();
const append = (l: string) => { log.push(l); }; // stable, as the shell's: a new identity re-runs the scan effect on every render
function Harness() {
  const connect = useConnectState();
  const [tab, setTab] = useState("connect");
  const model = useCameraConnection({
    ready: true, tab, setTab, append, ...connect, setCoreStatus: vi.fn(), setAfStatus: vi.fn(),
    setSamplePump: vi.fn(), setSheathPump: vi.fn(), setTrigStatus: vi.fn(), setReviewing: vi.fn(), recording: recordingNow, setRecording: setRecordingMock,
    tick: vi.fn(), stopLoop: vi.fn(), running: runningNow, expState: expStateNow,
  } as never);
  captured = { connect, model, tab, log };
  return null;
}
const mount = () => act(async () => root.render(<Harness />));
const camera = (label: string, p: Record<string, unknown> = {}) => ({ camera_type: 0, camera_index: -1, interface_index: 0, device_index: 0, interface_id: "", device_id: "", model_name: "", firmware_version: "", label, ...p });
const grabber = (label: string, device_index: number) => ({ interface_index: 0, device_index, stream_index: 0, interface_id: "", device_id: "", stream_id: "", model_name: "", label });
const scan = (p: Partial<CameraDiscovery> = {}): CameraDiscovery => ({ valid: true, complete: true, job_id: "1", cameras: [], framegrabbers: [], state: DISCOVERY_JOB_STATES.Completed, errors: [], ...p });
const selection = { valid: true, mode: 0, configured: false, running: false, label: "" };

describe("scanning", () => {
  beforeEach(() => { log.length = 0; vi.mocked(bridge.fetchCameraSelection).mockResolvedValue(selection as never); });

  it("Refresh joins the scan in flight, shows it, and ends with the found line and row 0 preselected", async () => {
    let finish!: (d: CameraDiscovery) => void;
    vi.mocked(bridge.discoverCameras).mockReturnValue(new Promise((r) => { finish = r; }));
    await mount();
    let a!: Promise<void>, b!: Promise<void>;
    await act(async () => { a = captured.model.refreshCameraState(); b = captured.model.refreshCameraState(); });
    expect(bridge.discoverCameras).toHaveBeenCalledOnce();
    expect(captured.connect.scanning).toBe(true);
    const d = scan({ cameras: [camera("Cam A"), camera("Cam B", { device_index: 1 })], framegrabbers: [grabber("FG", 0)] });
    await act(async () => { finish(d); await Promise.all([a, b]); });
    expect(captured.connect.scanning).toBe(false);
    expect(captured.connect.scanNote).toBe("Found 1 framegrabber(s), 2 eGrabber camera(s), 0 MindVision camera(s)");
    expect(captured.connect.pickedDevices.cameras).toBe(cameraKey(d.cameras[0]));
    expect(captured.connect.pickedDevices.framegrabbers).toBe(framegrabberKey(d.framegrabbers[0]));
    await act(async () => { await captured.model.refreshCameraState(); });
    expect(bridge.discoverCameras).toHaveBeenCalledTimes(2); // a finished scan does not block the next one
  });

  it("the Refresh button's scan clears the last action's note; other refreshes keep it", async () => {
    vi.mocked(bridge.discoverCameras).mockResolvedValue(scan());
    await mount();
    await act(async () => { captured.connect.setActionNote("Connected to Cam (not capturing)"); });
    await act(async () => { await captured.model.refreshCameraState(); });
    expect(captured.connect.actionNote).toBe("Connected to Cam (not capturing)");
    await act(async () => { await captured.model.refreshCameraState({ nativeEvent: {}, type: "click" }); });
    expect(captured.connect.actionNote).toBe("");
  });

  it("a refresh the code asks for does not scan under a running capture (the backend would refuse); the operator's Refresh does", async () => {
    vi.mocked(bridge.discoverCameras).mockResolvedValue(scan());
    runningNow = true;
    await mount();
    vi.mocked(bridge.discoverCameras).mockClear();
    await act(async () => { await captured.model.refreshCameraState(); });
    expect(bridge.discoverCameras).not.toHaveBeenCalled();
    expect(bridge.fetchCameraSelection).toHaveBeenCalled();
    await act(async () => { await captured.model.refreshCameraState({ nativeEvent: {} }); });
    expect(bridge.discoverCameras).toHaveBeenCalledOnce();
  });

  it("a refused scan (capturing) is on the status line, not only in the log", async () => {
    vi.mocked(bridge.discoverCameras).mockRejectedValue(new DiscoveryRefusedError("camera discovery is not allowed while capture is running"));
    await mount();
    await act(async () => { await captured.model.refreshCameraState(); });
    expect(captured.connect.scanNote).toBe("Camera discovery incomplete: camera discovery is not allowed while capture is running");
    expect(captured.connect.scanning).toBe(false);
    expect(log.some((l) => l.includes("discovery error"))).toBe(true);
  });

  it("a cancelled scan says so", async () => {
    vi.mocked(bridge.discoverCameras).mockResolvedValue(scan({ valid: false, complete: false, state: DISCOVERY_JOB_STATES.Cancelled }));
    await mount();
    await act(async () => { await captured.model.refreshCameraState(); });
    expect(captured.connect.scanNote).toBe("Camera scan cancelled.");
  });
});

describe("Connect and Configure Mock", () => {
  beforeEach(() => {
    log.length = 0;
    vi.mocked(bridge.fetchCameraSelection).mockResolvedValue(selection as never);
    vi.mocked(bridge.selectHardwareCamera).mockResolvedValue({ ok: true, message: "Hardware camera selected" } as never);
  });
  const withScan = async (d: CameraDiscovery) => {
    vi.mocked(bridge.discoverCameras).mockResolvedValue(d);
    await mount();
    await act(async () => { await captured.model.refreshCameraState(); });
  };

  it("selects the picked camera, says it, and moves to Overview", async () => {
    const d = scan({ cameras: [camera("Cam A"), camera("Cam B", { device_index: 1 })] });
    await withScan(d);
    await act(async () => { captured.connect.setPickedDevices((p) => ({ ...p, cameras: cameraKey(d.cameras[1]) })); });
    await act(async () => { await captured.model.onConnectPicked(); });
    expect(bridge.selectHardwareCamera).toHaveBeenCalledWith(0, 1, "Cam B");
    expect(captured.connect.actionNote).toBe("Connected to Cam B (not capturing)");
    expect(captured.tab).toBe("overview");
  });

  it("moves to Overview with the new selection at once; the device rescan runs behind it", async () => {
    const d = scan({ cameras: [camera("Cam A")] });
    await withScan(d);
    let finishScan!: (d: CameraDiscovery) => void;
    vi.mocked(bridge.discoverCameras).mockReturnValue(new Promise((r) => { finishScan = r; }));
    vi.mocked(bridge.fetchCameraSelection).mockResolvedValue({ ...selection, configured: true, label: "Cam A" } as never);
    await act(async () => { await captured.model.onConnectPicked(); });
    expect(captured.tab).toBe("overview");
    expect(captured.connect.camSelection?.configured).toBe(true); // Start Camera on Overview sees it before the scan ends
    expect(captured.connect.scanning).toBe(true);
    await act(async () => { finishScan(d); });
    expect(captured.connect.scanning).toBe(false);
    expect(captured.connect.actionNote).toBe("Connected to Cam A (not capturing)");
  });

  it("a framegrabber row selects that interface and device", async () => {
    const d = scan({ framegrabbers: [grabber("Coaxlink 0", 0), { ...grabber("Coaxlink 1", 1), interface_index: 2 }] });
    await withScan(d);
    await act(async () => { captured.connect.setConnectTab("framegrabbers"); captured.connect.setPickedDevices((p) => ({ ...p, framegrabbers: framegrabberKey(d.framegrabbers[1]) })); });
    await act(async () => { await captured.model.onConnectPicked(); });
    expect(bridge.selectHardwareCamera).toHaveBeenCalledWith(2, 1, "Coaxlink 1");
    expect(captured.tab).toBe("overview");
  });

  it("acts on the shown sub-tab's pick, not on a hidden one", async () => {
    const d = scan({ cameras: [camera("Cam A")], framegrabbers: [grabber("FG", 0)] });
    await withScan(d);
    await act(async () => { captured.connect.setConnectTab("mindvision"); });
    await act(async () => { await captured.model.onConnectPicked(); });
    expect(bridge.selectHardwareCamera).not.toHaveBeenCalled();
    expect(bridge.selectMindVisionCamera).not.toHaveBeenCalled();
  });

  it("a picked device that a rescan removed says so and rescans (App.tsx:703)", async () => {
    const d = scan({ cameras: [camera("Cam A")] });
    await withScan(d);
    await act(async () => { captured.connect.setPickedDevices((p) => ({ ...p, cameras: "0:9:9:9" })); });
    await act(async () => { await captured.model.onConnectPicked(); });
    expect(bridge.selectHardwareCamera).not.toHaveBeenCalled();
    expect(log.some((l) => l.includes("no longer listed"))).toBe(true);
    expect(bridge.discoverCameras).toHaveBeenCalledTimes(3); // the scan at mount, the test's, the rescan the message announces
  });

  it("a refusal (the Rust guard while capturing) is a modal warning and the stage stays", async () => {
    await withScan(scan({ cameras: [camera("Cam A")] }));
    vi.mocked(bridge.selectHardwareCamera).mockRejectedValue("Stop capture and finalize the experiment before changing camera settings");
    await act(async () => { await captured.model.onConnectPicked(); });
    expect(captured.connect.cameraNotice).toEqual({ title: "Connect Device", message: "Stop capture and finalize the experiment before changing camera settings" });
    expect(captured.tab).toBe("connect");
    vi.mocked(bridge.selectHardwareCamera).mockResolvedValue({ ok: false, message: "Invalid hardware camera indices" } as never);
    await act(async () => { await captured.model.onConnectPicked(); });
    expect(captured.connect.cameraNotice?.message).toBe("Invalid hardware camera indices");
  });

  it("Configure Mock: says 'Mock camera configured (not capturing)' and moves to Overview; a refusal warns", async () => {
    await withScan(scan());
    vi.mocked(bridge.configureMockFps).mockResolvedValue({ ok: true, message: "Mock camera configured" } as never);
    await act(async () => { captured.connect.setFrameDir("  /frames  "); });
    await act(async () => { await captured.model.onConfigureMock(); });
    expect(bridge.configureMockFps).toHaveBeenCalledWith("/frames", 5000, true); // trimmed, the rig's 5000 fps
    expect(captured.connect.actionNote).toBe("Mock camera configured (not capturing)");
    expect(captured.tab).toBe("overview");
    expect(captured.connect.showMockConfig).toBe(false);

    await act(async () => { captured.connect.setActionNote(""); });
    vi.mocked(bridge.configureMockFps).mockResolvedValue({ ok: false, message: "Cannot change camera connection while camera is running. Please stop the camera first." } as never);
    await act(async () => { captured.connect.setShowMockConfig(true); });
    await act(async () => { await captured.model.onConfigureMock(); });
    expect(captured.connect.cameraNotice?.title).toBe("Configure Mock Camera");
    expect(captured.connect.cameraNotice?.message).toMatch(/camera is running/);
  });
});

describe("delivery mode", () => {
  it("sets the mode, shows the backend's wording, and re-reads the status", async () => {
    log.length = 0;
    vi.mocked(bridge.setFrameDeliveryMode).mockResolvedValue({ ok: true, message: "Delivery mode set to Latest Frame." });
    vi.mocked(bridge.fetchFrameDeliveryMode).mockResolvedValue({ valid: true, mode: "latestFrame", active_mode: "everyFrame", confirmed: false, capturing: false });
    await mount();
    await act(async () => { await captured.model.onDeliveryModeChange("latestFrame"); });
    expect(bridge.setFrameDeliveryMode).toHaveBeenCalledWith("latestFrame");
    expect(captured.connect.actionNote).toBe("Delivery mode set to Latest Frame.");
    expect(captured.connect.deliveryModeStatus?.mode).toBe("latestFrame");
  });
  it("a failure is on the status line", async () => {
    vi.mocked(bridge.setFrameDeliveryMode).mockRejectedValue("VIEWER_ONLY");
    await mount();
    await act(async () => { await captured.model.onDeliveryModeChange("everyFrame"); });
    expect(captured.connect.actionNote).toBe("Delivery mode: VIEWER_ONLY");
  });
});

describe("Start and Stop Camera (CONNECT-033/036/038/039/040)", () => {
  beforeEach(() => { log.length = 0; vi.mocked(bridge.startCapture).mockResolvedValue({ ok: true, message: "Capture started" } as never); });
  const settled = (resolve: (r: unknown) => void) => resolve({ ok: true, message: "" });

  it("start: one command in flight; a duplicate click, key press or shortcut sends no second backend command", async () => {
    let finish!: (r: unknown) => void;
    vi.mocked(bridge.startCapture).mockReturnValue(new Promise((r) => { finish = r; }) as never);
    await mount();
    let first!: Promise<void>, second!: Promise<void>;
    await act(async () => { first = captured.model.onStartCamera(); second = captured.model.onStartCamera(); });
    expect(bridge.startCapture).toHaveBeenCalledOnce();
    expect(captured.connect.cameraCommand).toBe("start");
    await act(async () => { await captured.model.onStopCamera(); }); // a Stop while the Start is in flight is not sent either
    expect(bridge.stopCapture).not.toHaveBeenCalled();
    await act(async () => { settled(finish); await Promise.all([first, second]); });
    expect(captured.connect.cameraCommand).toBeNull();
    expect(captured.connect.running).toBe(true);
    // the command is free again
    await act(async () => { await captured.model.onStartCamera(); });
    expect(bridge.startCapture).toHaveBeenCalledTimes(2);
  });

  it("start: a successful start lands the operator on Overview", async () => {
    await mount();
    await act(async () => { await captured.model.onStartCamera(); });
    expect(captured.tab).toBe("overview");
  });

  it("start: a refused or failed Start raises the warning titled 'Camera' with the reason, and frees the command", async () => {
    vi.mocked(bridge.startCapture).mockResolvedValue({ ok: false, message: "Capture start failed" } as never);
    await mount();
    await act(async () => { await captured.model.onStartCamera(); });
    expect(captured.connect.cameraNotice).toEqual({ title: "Camera", message: "Capture start failed" });
    expect(captured.connect.actionNote).toBe("Capture start failed");
    expect(captured.connect.cameraCommand).toBeNull();
    expect(captured.connect.running).toBe(false);
    expect(captured.tab).toBe("connect");
    vi.mocked(bridge.startCapture).mockRejectedValue("VIEWER_ONLY");
    await act(async () => { captured.connect.setCameraNotice(null); await captured.model.onStartCamera(); });
    expect(captured.connect.cameraNotice).toEqual({ title: "Camera", message: "VIEWER_ONLY" });
  });

  it("stop: sends one command; a duplicate sends none", async () => {
    runningNow = true;
    let finish!: (r: unknown) => void;
    vi.mocked(bridge.stopCapture).mockReturnValue(new Promise((r) => { finish = r; }) as never);
    await mount();
    let a!: Promise<void>, b!: Promise<void>;
    await act(async () => { a = captured.model.onStopCamera(); b = captured.model.onStopCamera(); });
    expect(bridge.stopCapture).toHaveBeenCalledOnce();
    await act(async () => { settled(finish); await Promise.all([a, b]); });
    expect(captured.connect.running).toBe(false);
    expect(log).toContain("capture stopped");
  });

  it("stop: never while an experiment runs or saves; the warning says why and nothing is sent (CONNECT-038)", async () => {
    runningNow = true;
    await mount();
    for (const [state, text] of [[1, /experiment is starting/], [2, /experiment is active/], [3, /data is being saved/]] as const) {
      expStateNow = state;
      await mount();
      await act(async () => { captured.connect.setCameraNotice(null); await captured.model.onStopCamera(); });
      expect(captured.connect.cameraNotice?.title).toBe("Camera");
      expect(captured.connect.cameraNotice?.message).toMatch(text);
    }
    expect(bridge.stopCapture).not.toHaveBeenCalled();
  });

  it("stop: a failed device teardown is a failed command with the camera's message (CONNECT-040)", async () => {
    runningNow = true;
    vi.mocked(bridge.stopCapture).mockResolvedValue({ ok: false, message: "AcquisitionStop was not confirmed by the generator" } as never);
    await mount();
    await act(async () => { await captured.model.onStopCamera(); });
    expect(captured.connect.cameraNotice).toEqual({ title: "Camera", message: "AcquisitionStop was not confirmed by the generator" });
    expect(captured.connect.cameraCommand).toBeNull();
  });

  it("stop with a raw recording running: the button finishes the recording first, then stops (CONNECT-039 decision)", async () => {
    runningNow = true; recordingNow = true;
    vi.mocked(bridge.stopRecording).mockResolvedValue({ ok: true, message: "" } as never);
    vi.mocked(bridge.stopCapture).mockResolvedValue({ ok: true, message: "" } as never);
    await mount();
    await act(async () => { await captured.model.onStopCamera(); });
    expect(bridge.stopRecording).toHaveBeenCalledOnce();
    expect(setRecordingMock).toHaveBeenCalledWith(false);
    expect(bridge.stopCapture).toHaveBeenCalledOnce();
    expect(vi.mocked(bridge.stopRecording).mock.invocationCallOrder[0]).toBeLessThan(vi.mocked(bridge.stopCapture).mock.invocationCallOrder[0]);
    expect(log).toContain("recording finalised; capture stopped");
  });

  it("stop with a raw recording that cannot be closed: the camera keeps running and the warning says so", async () => {
    runningNow = true; recordingNow = true;
    vi.mocked(bridge.stopRecording).mockResolvedValue({ ok: false, message: "disk full" } as never);
    await mount();
    await act(async () => { await captured.model.onStopCamera(); });
    expect(bridge.stopCapture).not.toHaveBeenCalled();
    expect(captured.connect.cameraNotice).toEqual({ title: "Camera", message: "Stop recording failed: disk full" });
    expect(captured.connect.cameraCommand).toBeNull();
  });
});

describe("Configure Mock defaults (CONNECT-014/016)", () => {
  beforeEach(() => {
    log.length = 0;
    // An in-memory store, as the other storage tests use: Node 25+ defines its own global localStorage, which
    // jsdom does not replace, and it is unusable without --localstorage-file.
    const saved = new Map<string, string>();
    vi.stubGlobal("localStorage", {
      getItem: (key: string) => saved.get(key) ?? null, setItem: (key: string, value: string) => { saved.set(key, String(value)); },
      removeItem: (key: string) => { saved.delete(key); }, clear: () => saved.clear(),
    });
    vi.mocked(bridge.fetchCameraSelection).mockResolvedValue(selection as never);
    vi.mocked(bridge.discoverCameras).mockResolvedValue(scan());
    vi.mocked(bridge.appPaths).mockResolvedValue({ documents: "/home/op/Documents" } as never);
  });
  const open = async () => { await mount(); await act(async () => { captured.connect.setShowMockConfig(true); }); };

  it("opens on Documents when nothing was used before (Qt: ../data/mock_frames next to the app, else Documents)", async () => {
    await open();
    expect(captured.connect.frameDir).toBe("/home/op/Documents");
    expect(captured.connect.mockFps).toBe("5000");
    expect(captured.connect.loopFiles).toBe(true);
  });

  it("prefers the folder the backend's mock source holds (MIB_MOCK_CAMERA_DIR), then the one used last time", async () => {
    await mount();
    await act(async () => { captured.connect.setCamSelection({ ...selection, configured: true, mode: CAMERA_SELECTION_MODES.Mock, mock_frame_dir: "/data/mock_frames" } as never); });
    await act(async () => { captured.connect.setShowMockConfig(true); });
    expect(captured.connect.frameDir).toBe("/data/mock_frames");
  });

  it("starts from what was used last time, remembered across sessions", async () => {
    localStorage.setItem("mib.mock-camera.v1", JSON.stringify({ dir: "/last", fps: "250", loop: false }));
    await open();
    expect(captured.connect.frameDir).toBe("/last");
    expect(captured.connect.mockFps).toBe("250");
    expect(captured.connect.loopFiles).toBe(false);
    expect(bridge.appPaths).not.toHaveBeenCalled();
  });

  it("applying remembers the folder (trimmed) and the rate", async () => {
    vi.mocked(bridge.configureMockFps).mockResolvedValue({ ok: true, message: "" } as never);
    await open();
    await act(async () => { captured.connect.setFrameDir(" /frames "); captured.connect.setMockFps("2500"); });
    await act(async () => { await captured.model.onConfigureMock(); });
    expect(bridge.configureMockFps).toHaveBeenCalledWith("/frames", 2500, true);
    expect(JSON.parse(localStorage.getItem("mib.mock-camera.v1")!)).toEqual({ dir: "/frames", fps: "2500", loop: true });
    expect(captured.connect.frameDir).toBe("/frames");
  });

  it("a blank folder or a rate outside 1 to 10000 sends nothing", async () => {
    await open();
    await act(async () => { captured.connect.setFrameDir("   "); });
    await act(async () => { await captured.model.onConfigureMock(); });
    await act(async () => { captured.connect.setFrameDir("/f"); captured.connect.setMockFps("10001"); });
    await act(async () => { await captured.model.onConfigureMock(); });
    expect(bridge.configureMockFps).not.toHaveBeenCalled();
  });
});
