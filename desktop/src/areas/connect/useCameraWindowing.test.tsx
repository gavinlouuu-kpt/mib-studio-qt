// @vitest-environment jsdom
// #863 (CONNECT-053, App.tsx:1096): the Overview polls the Recording capacity once a second with the camera
// window box as the planned window, keeps the last view while the backend is busy, stops during an
// experiment; and a geometry the backend cannot give is an error line, not a silent null.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { CameraGeometry, RecordingCapacity } from "../../bridge";
import { NO_SOURCES } from "./cameraStage";

const bridgeMock = vi.hoisted(() => ({
  fetchCameraGeometry: vi.fn(), fetchRecordingCapacity: vi.fn(), fetchRunFps: vi.fn(), saveCameraWindow: vi.fn(),
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
