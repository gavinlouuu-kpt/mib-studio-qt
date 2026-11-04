// @vitest-environment jsdom
// #863 (App.tsx:1096): a geometry the backend cannot give is an error line in the Overview, not a silent null.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { CameraGeometry } from "../../bridge";
import { NO_SOURCES } from "./cameraStage";

const bridgeMock = vi.hoisted(() => ({
  fetchCameraGeometry: vi.fn(), fetchRunFps: vi.fn(), saveCameraWindow: vi.fn(),
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

type Props = { tab: string; expActive?: boolean; instrumentModes?: boolean };
let host: HTMLDivElement, root: Root;
let log: string[], geometryErrors: string[], geometries: CameraGeometry[];
let geometryRef: { current: CameraGeometry | null }, windowRef: { current: { x: number; y: number; width: number; height: number } | null };
let windowing: ReturnType<typeof useCameraWindowing> | null;

function Harness({ tab, expActive = false, instrumentModes = false }: Props) {
  windowing = useCameraWindowing({
    ready: true, tab: tab as never, instrumentModes, append: (line: string) => log.push(line),
    cameraGeometryRef: geometryRef, cameraWindowRef: windowRef, refreshCameraGeometryRef: { current: async () => undefined },
    setCameraGeometry: (g: CameraGeometry | null) => { if (g) geometries.push(g); }, setRunFpsStatus: () => undefined, expActive,
    scriptSourcesRef: { current: NO_SOURCES }, setGeometryError: (text: string) => geometryErrors.push(text),
  } as never);
  return null;
}

const render = async (props: Props) => { await act(async () => { root.render(<Harness {...props} />); }); };

beforeEach(() => {
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
  log = []; geometryErrors = []; geometries = []; windowing = null;
  geometryRef = { current: geometry() }; windowRef = { current: { x: 368, y: 616, width: 1216, height: 208 } };
  for (const fn of Object.values(bridgeMock)) fn.mockReset();
  bridgeMock.fetchCameraGeometry.mockImplementation(async () => geometry());
  bridgeMock.fetchRunFps.mockResolvedValue(null);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

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
