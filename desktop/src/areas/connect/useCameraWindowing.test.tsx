// @vitest-environment jsdom
// #863 (CONNECT-053, App.tsx:1096): the Overview polls the Recording capacity once a second with the camera
// window box as the planned window, keeps the last view while the backend is busy, stops during an
// experiment; and a geometry the backend cannot give is an error line, not a silent null.
import { act, useState } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { CameraGeometry, CameraSelection, RecordingCapacity } from "../../bridge";
import { NO_SOURCES } from "./cameraStage";

const bridgeMock = vi.hoisted(() => ({
  fetchCameraGeometry: vi.fn(), fetchRecordingCapacity: vi.fn(), fetchRunFps: vi.fn(), saveCameraWindow: vi.fn(), fetchFrame: vi.fn(),
}));
vi.mock("../../bridge", () => ({ bridge: bridgeMock }));
vi.mock("../../transport", () => ({ invoke: vi.fn() }));
import { useCameraWindowing } from "./useConnect";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

const geometry = (over: Partial<CameraGeometry> = {}): CameraGeometry => ({
  supported: true, overview: true, camera: "egrabber", sensor_width: 1920, sensor_height: 1080,
  roi: { x: 368, y: 616, width: 1216, height: 208 }, width_increment: 16, height_increment: 4, offset_x_increment: 16,
  offset_y_increment: 4, min_width: 64, min_height: 16, session: {}, window_source: "profile", ...over,
} as CameraGeometry);
const capacity = (over: Partial<RecordingCapacity> = {}): RecordingCapacity => ({ busy: false, known: true, planned: true, status: "ok", gates: [], ...over });

type Props = { tab: string; expActive?: boolean; hostProcessing?: boolean; instrumentModes?: boolean };
let host: HTMLDivElement, root: Root;
let log: string[], capacities: RecordingCapacity[], geometryErrors: string[], geometries: CameraGeometry[];
let geometryRef: { current: CameraGeometry | null }, windowRef: { current: { x: number; y: number; width: number; height: number } | null };
let windowing: ReturnType<typeof useCameraWindowing> | null;

function Harness({ tab, expActive = false, hostProcessing = true, instrumentModes = false }: Props) {
  windowing = useCameraWindowing({
    ready: true, tab: tab as never, hostProcessing, instrumentModes, append: (line: string) => log.push(line),
    cameraGeometryRef: geometryRef, cameraWindowRef: windowRef, refreshCameraGeometryRef: { current: async () => undefined },
    setCameraGeometry: (g: CameraGeometry | null) => { if (g) geometries.push(g); }, setRunFpsStatus: () => undefined, expActive,
    scriptSourcesRef: { current: NO_SOURCES }, setGeometryError: (text: string) => geometryErrors.push(text),
    setCapacity: (view: RecordingCapacity | null) => { if (view) capacities.push(view); },
    camSelection: null, roiOverlay: false, setRoiOverlay: () => undefined, roiOverlayRef: { current: false },
    framePulls: { current: { request: () => undefined } },
  } as never);
  return null;
}

const render = async (props: Props) => { await act(async () => { root.render(<Harness {...props} />); }); };
const tick = async (ms: number) => { await act(async () => { await vi.advanceTimersByTimeAsync(ms); }); };

beforeEach(() => {
  vi.useFakeTimers();
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
  log = []; capacities = []; geometryErrors = []; geometries = []; windowing = null;
  geometryRef = { current: geometry() }; windowRef = { current: { x: 368, y: 616, width: 1216, height: 208 } };
  for (const fn of Object.values(bridgeMock)) fn.mockReset();
  bridgeMock.fetchRecordingCapacity.mockImplementation(async () => capacity());
  bridgeMock.fetchCameraGeometry.mockImplementation(async () => geometry());
  bridgeMock.fetchRunFps.mockResolvedValue(null);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.useRealTimers(); });

describe("the Recording capacity poll on the Overview", () => {
  it("asks at once and then every second, with the camera window box as the planned window", async () => {
    await render({ tab: "connect" });
    await act(async () => { windowing!.setCameraWindow({ x: 368, y: 616, width: 1216, height: 208 }); }); // what the geometry gave
    await render({ tab: "overview" });
    await tick(0);
    expect(bridgeMock.fetchRecordingCapacity).toHaveBeenCalledTimes(1);
    expect(bridgeMock.fetchRecordingCapacity).toHaveBeenLastCalledWith({ x: 368, y: 616, width: 1216, height: 208 });
    await act(async () => { windowing!.setCameraWindow({ x: 704, y: 500, width: 512, height: 96 }); }); // the operator moved the box
    await tick(1000);
    expect(bridgeMock.fetchRecordingCapacity).toHaveBeenCalledTimes(2);
    expect(bridgeMock.fetchRecordingCapacity).toHaveBeenLastCalledWith({ x: 704, y: 500, width: 512, height: 96 });
    await tick(2000);
    expect(bridgeMock.fetchRecordingCapacity).toHaveBeenCalledTimes(4);
    expect(capacities.length).toBe(4);
  });

  it("describes the next run, not the planned box, when the camera has no window box", async () => {
    geometryRef.current = geometry({ supported: false });
    await render({ tab: "overview" });
    await tick(0);
    expect(bridgeMock.fetchRecordingCapacity).toHaveBeenCalledWith(undefined);
  });

  it("keeps the last view while the backend is busy, and when a poll fails", async () => {
    await render({ tab: "overview" });
    await tick(0);
    expect(capacities.length).toBe(1);
    bridgeMock.fetchRecordingCapacity.mockResolvedValueOnce(capacity({ busy: true, known: false, status: "unknown" }));
    await tick(1000);
    bridgeMock.fetchRecordingCapacity.mockRejectedValueOnce(new Error("backend gone"));
    await tick(1000);
    expect(bridgeMock.fetchRecordingCapacity).toHaveBeenCalledTimes(3);
    expect(capacities.length).toBe(1); // neither the busy reply nor the failure replaced the view
    await tick(1000);
    expect(capacities.length).toBe(2);
  });

  it("is frozen during an experiment, and silent on the other stages, the PZ7035 and a PL instrument", async () => {
    await render({ tab: "overview", expActive: true });
    await render({ tab: "experiment" });
    await render({ tab: "connect" });
    await render({ tab: "overview", instrumentModes: true });
    await render({ tab: "overview", hostProcessing: false });
    await tick(3000);
    expect(bridgeMock.fetchRecordingCapacity).not.toHaveBeenCalled();
  });

  it("stops polling when the operator leaves the Overview", async () => {
    await render({ tab: "overview" });
    await tick(1000);
    const calls = bridgeMock.fetchRecordingCapacity.mock.calls.length;
    await render({ tab: "experiment" });
    await tick(5000);
    expect(bridgeMock.fetchRecordingCapacity.mock.calls.length).toBe(calls);
  });
});

describe("a geometry the backend cannot give (App.tsx:1096 swallowed it)", () => {
  it("is an error line and one log entry per change, cleared by the next good read", async () => {
    await render({ tab: "overview" });
    bridgeMock.fetchCameraGeometry.mockRejectedValue(new Error("backend busy"));
    let result: CameraGeometry | null | undefined;
    await act(async () => { result = await windowing!.refreshCameraGeometry(); });
    await act(async () => { await windowing!.refreshCameraGeometry(); });
    expect(result).toBeNull();
    expect(geometryErrors).toEqual(["Camera geometry unavailable: backend busy", "Camera geometry unavailable: backend busy"]);
    expect(log).toEqual(["Camera geometry unavailable: backend busy"]); // the 2 s poll does not flood the log
    bridgeMock.fetchCameraGeometry.mockResolvedValue(geometry());
    await act(async () => { result = await windowing!.refreshCameraGeometry(); });
    expect(result?.camera).toBe("egrabber");
    expect(geometryErrors[geometryErrors.length - 1]).toBe("");
    bridgeMock.fetchCameraGeometry.mockRejectedValue(new Error("backend busy"));
    await act(async () => { await windowing!.refreshCameraGeometry(); });
    expect(log.length).toBe(2); // a new failure after a good read is logged again
  });
});


// ---- CONNECT-047: the Overview's ROI Overlay toggle ----
const selected = (over: Partial<CameraSelection> = {}): CameraSelection => ({
  valid: true, configured: true, mode: 2, running: false, interface_index: 0, device_index: 0, label: "MC2066", mindvision_index: -1,
  mindvision_config_path: "", camera_script_path: "", mock_frame_dir: "", mock_interval_ms: 0, mock_loop: false, ...over,
});
let overlayRef: { current: boolean }, overlayOn: boolean, pulls: string[];
function OverlayHarness({ selection, instrumentModes = false }: { selection: CameraSelection | null; instrumentModes?: boolean }) {
  const [on, setOn] = useState(false);
  overlayOn = on;
  windowing = useCameraWindowing({
    ready: true, tab: "overview" as never, hostProcessing: true, instrumentModes, append: (line: string) => log.push(line),
    cameraGeometryRef: geometryRef, cameraWindowRef: windowRef, refreshCameraGeometryRef: { current: async () => undefined },
    setCameraGeometry: () => undefined, setRunFpsStatus: () => undefined, expActive: false, scriptSourcesRef: { current: NO_SOURCES },
    setGeometryError: () => undefined, setCapacity: () => undefined, camSelection: selection, roiOverlay: on, setRoiOverlay: setOn, roiOverlayRef: overlayRef,
    framePulls: { current: { request: (id: string) => pulls.push(id) } },
  } as never);
  return null;
}
const renderOverlay = async (props: { selection: CameraSelection | null; instrumentModes?: boolean }) => { await act(async () => { root.render(<OverlayHarness {...props} />); }); };

describe("the ROI Overlay (CONNECT-047)", () => {
  beforeEach(() => { overlayRef = { current: false }; pulls = []; });

  it("is off with no camera, switches on when a camera is connected, and stays as the operator left it on the next polls", async () => {
    await renderOverlay({ selection: null });
    expect(overlayOn).toBe(false);
    expect(overlayRef.current).toBe(false);
    await renderOverlay({ selection: selected({ configured: false }) });
    expect(overlayOn).toBe(false);
    await renderOverlay({ selection: selected() });
    expect(overlayOn).toBe(true);
    expect(overlayRef.current).toBe(true);        // what draw() and the drag read
    await act(async () => { windowing!.toggleRoiOverlay(); });
    expect(overlayOn).toBe(false);
    expect(overlayRef.current).toBe(false);
    await renderOverlay({ selection: selected({ running: true }) });  // the poll after Start Camera: the same camera
    expect(overlayOn).toBe(false);
    await renderOverlay({ selection: selected({ running: true, device_index: 1, label: "MC2067" }) }); // another camera
    expect(overlayOn).toBe(true);
  });

  it("keeps the choice when the camera disconnects", async () => {
    await renderOverlay({ selection: selected() });
    await renderOverlay({ selection: null });
    expect(overlayOn).toBe(true);
    await act(async () => { windowing!.toggleRoiOverlay(); });
    await renderOverlay({ selection: null });
    expect(overlayOn).toBe(false);
  });

  it("asks for the last frame when it is toggled, so a stopped camera shows the box at once", async () => {
    await renderOverlay({ selection: selected() });
    expect(pulls).toEqual([]);
    await act(async () => { windowing!.toggleRoiOverlay(); });
    expect(pulls).toEqual(["live"]);
  });

  it("always shows the PZ7035's Run window: there is no toggle for it", async () => {
    await renderOverlay({ selection: null, instrumentModes: true });
    expect(overlayOn).toBe(false);
    expect(overlayRef.current).toBe(true);
  });
});
