// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { CameraGeometry } from "./bridge";
import { restoredRunWindow, runModeBlockReason, useCameraWindow } from "./cameraWindow";

const saveCameraRoi = vi.fn(async (_x: number, _y: number, _w: number, _h: number) => ({ ok: true, message: "ROI saved" }));
vi.mock("./bridge", () => ({ bridge: { saveCameraRoi: (x: number, y: number, w: number, h: number) => saveCameraRoi(x, y, w, h) } }));

const geometry = (over: Partial<CameraGeometry> = {}): CameraGeometry => ({
  supported: true, overview: true, sensor_width: 816, sensor_height: 624, min_width: 8, min_height: 4,
  width_increment: 8, height_increment: 4, offset_x_increment: 8, offset_y_increment: 4,
  roi: { x: 0, y: 0, width: 0, height: 0 }, session: {}, ...over,
} as CameraGeometry);

describe("pure helpers", () => {
  it("restores the window the backend applied, snapped to the Run grid, and only once a Run switch has happened", () => {
    expect(restoredRunWindow(undefined)).toBeNull();
    expect(restoredRunWindow({ name: "align", run_x: 0, run_y: 0, service: false, run_set: false })).toBeNull();
    expect(restoredRunWindow({ name: "align", run_x: 152, run_y: 200, service: false })).toBeNull(); // an older backend
    expect(restoredRunWindow({ name: "run", run_x: 157, run_y: 203, service: false, run_set: true })).toEqual({ x: 160, y: 204, width: 512, height: 96 });
  });

  it("explains a blocked Run in order: no window, then a refused switch, then not in Run yet", () => {
    const err = { mode: "run" as const, message: "Stop the experiment or recording before changing camera mode" };
    expect(runModeBlockReason({ windowPlaced: false, modeError: err, runMode: false })).toContain("Place the 512×96 run window");
    expect(runModeBlockReason({ windowPlaced: true, modeError: err, runMode: false })).toContain("did not switch to Run: Stop the experiment");
    expect(runModeBlockReason({ windowPlaced: true, modeError: null, runMode: false })).toContain("not in Run mode");
    expect(runModeBlockReason({ windowPlaced: true, modeError: null, runMode: true })).toBeUndefined();
    // an Align refusal does not block Run
    expect(runModeBlockReason({ windowPlaced: true, modeError: { mode: "align", message: "x" }, runMode: true })).toBeUndefined();
  });
});

type Api = ReturnType<typeof useCameraWindow>;
let host: HTMLDivElement, root: Root, api: Api, log: string[], refreshed: number;
const geometryRef = { current: geometry() as CameraGeometry | null };

function Harness({ instrumentModes, expActive = false }: { instrumentModes: boolean; expActive?: boolean }) {
  api = useCameraWindow({ instrumentModes, geometryRef, expActive, append: (l) => log.push(l), refreshGeometry: async () => { refreshed++; } });
  return null;
}
const fakeEvent = (x: number, y: number) => ({
  clientX: x, clientY: y, pointerId: 1,
  currentTarget: { width: 816, height: 624, getBoundingClientRect: () => ({ left: 0, top: 0, width: 816, height: 624 }), setPointerCapture: () => undefined },
}) as never;

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
  log = []; refreshed = 0; saveCameraRoi.mockClear(); geometryRef.current = geometry();
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

describe("the camera window after a page reload (PZ7035)", () => {
  it("shows the default window without calling it placed", async () => {
    await act(async () => root.render(<Harness instrumentModes />));
    act(() => api.showDefault(geometry()));
    expect(api.window).toEqual({ x: 152, y: 264, width: 512, height: 96 });
    expect(api.placed).toBe(false);
  });

  it("takes the window the backend applied, over the default, and marks it placed", async () => {
    await act(async () => root.render(<Harness instrumentModes />));
    act(() => api.showDefault(geometry()));
    act(() => api.restore({ name: "run", run_x: 232, run_y: 336, service: false, run_set: true }));
    expect(api.window).toEqual({ x: 232, y: 336, width: 512, height: 96 });
    expect(api.placed).toBe(true);
  });

  it("falls back to 'not placed' when the backend has no window", async () => {
    await act(async () => root.render(<Harness instrumentModes />));
    act(() => api.showDefault(geometry()));
    act(() => api.restore({ name: "align", run_x: 0, run_y: 0, service: false, run_set: false }));
    expect(api.placed).toBe(false);
    expect(api.window).toEqual({ x: 152, y: 264, width: 512, height: 96 });
  });

  it("does not overwrite a window the operator placed after the page loaded", async () => {
    await act(async () => root.render(<Harness instrumentModes />));
    act(() => api.showDefault(geometry()));
    await act(async () => { await api.save({ x: 40, y: 100, width: 512, height: 96 }); });
    expect(api.placed).toBe(true);
    act(() => api.restore({ name: "run", run_x: 232, run_y: 336, service: false, run_set: true }));
    expect(api.window).toEqual({ x: 40, y: 100, width: 512, height: 96 });
  });
});

describe("saving the window on release", () => {
  it("PZ7035: a release places the snapped Run window and sends nothing to the camera", async () => {
    await act(async () => root.render(<Harness instrumentModes />));
    act(() => api.showDefault(geometry())); // (152, 264)
    act(() => api.onPointerDown(fakeEvent(200, 300)));
    act(() => api.onPointerMove(fakeEvent(213, 313)));  // moved by (13, 13): snaps to x % 8, y % 4
    await act(async () => api.onPointerUp());
    expect(api.window).toEqual({ x: 168, y: 276, width: 512, height: 96 });
    expect(api.placed).toBe(true);
    expect(saveCameraRoi).not.toHaveBeenCalled();
    expect(log.join(" ")).toContain("applied when Experiment opens");
  });

  it("other cameras (Aravis/MindVision): a release saves the snapped ROI and re-reads the geometry", async () => {
    await act(async () => root.render(<Harness instrumentModes={false} />));
    act(() => api.showDefault(geometry({ roi: { x: 100, y: 120, width: 256, height: 128 } })));
    act(() => api.onPointerDown(fakeEvent(150, 150)));
    act(() => api.onPointerMove(fakeEvent(163, 161)));
    expect(saveCameraRoi).not.toHaveBeenCalled(); // nothing is sent while dragging
    await act(async () => api.onPointerUp());
    expect(saveCameraRoi).toHaveBeenCalledTimes(1);
    expect(saveCameraRoi).toHaveBeenCalledWith(112, 132, 256, 128); // moved by (13, 11), snapped to 8 / 4
    expect(refreshed).toBe(1);
    expect(log.join(" ")).toContain("ROI saved");
  });

  it("ignores a press outside the window, and any drag during a run", async () => {
    await act(async () => root.render(<Harness instrumentModes={false} />));
    act(() => api.showDefault(geometry({ roi: { x: 100, y: 120, width: 256, height: 128 } })));
    act(() => api.onPointerDown(fakeEvent(5, 5)));
    await act(async () => api.onPointerUp());
    expect(saveCameraRoi).not.toHaveBeenCalled();
    await act(async () => root.render(<Harness instrumentModes={false} expActive />));
    act(() => api.onPointerDown(fakeEvent(150, 150)));
    await act(async () => api.onPointerUp());
    expect(saveCameraRoi).not.toHaveBeenCalled();
  });
});
