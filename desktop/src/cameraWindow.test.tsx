// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { CameraGeometry, SaveCameraWindowReply } from "./bridge";
import { describeSavedWindow, restoredRunWindow, runModeBlockReason, useCameraWindow, WINDOW_SAVE_DELAY_MS } from "./cameraWindow";

const saveCameraRoi = vi.fn(async (_x: number, _y: number, _w: number, _h: number) => ({ ok: true, message: "ROI saved" }));
const saveCameraWindow = vi.fn(async (_path: string, _x: number, _y: number, _w: number, _h: number): Promise<SaveCameraWindowReply> => { throw new Error("not mocked"); });
vi.mock("./bridge", () => ({ bridge: {
  saveCameraRoi: (x: number, y: number, w: number, h: number) => saveCameraRoi(x, y, w, h),
  saveCameraWindow: (path: string, x: number, y: number, w: number, h: number) => saveCameraWindow(path, x, y, w, h),
} }));

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

let scriptPath = "";
function Harness({ instrumentModes, expActive = false }: { instrumentModes: boolean; expActive?: boolean }) {
  api = useCameraWindow({
    instrumentModes, geometryRef, expActive, append: (l) => log.push(l), refreshGeometry: async () => { refreshed++; },
    resolveScriptPath: async () => scriptPath,
  });
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


// ---- the eGrabber window (#846, #863): Qt snapping, saved into the script and the profile ----
const egrabber = (over: Partial<CameraGeometry> = {}): CameraGeometry => ({
  supported: true, overview: true, camera: "egrabber", sensor_width: 1920, sensor_height: 1080, min_width: 64, min_height: 16,
  width_increment: 16, height_increment: 4, offset_x_increment: 16, offset_y_increment: 4,
  roi: { x: 368, y: 616, width: 1216, height: 208 }, window_source: "profile", session: {}, ...over,
} as CameraGeometry);
const reply = (over: Partial<SaveCameraWindowReply> = {}): SaveCameraWindowReply => ({
  ok: true, error: "", requested: { x: 371, y: 203, width: 1000, height: 130 }, window: { x: 368, y: 200, width: 992, height: 128 }, snapped: true,
  sensor_width: 1920, sensor_height: 1080,
  script: { path: "C:\\MIBTest\\include\\egrabberConfig.js", updated: true, changed: true, missing: [] },
  profile: { config_has_window: true, local_profile: true, name: "lab", memory_updated: true, config_updated: true, reason: "" }, ...over,
});

describe("saving an eGrabber window", () => {
  it("says what was saved, what it was snapped from, and where it went", () => {
    const said = describeSavedWindow(reply());
    expect(said.level).toBe("info");
    expect(said.text).toBe("Camera window 992×128 at (368, 200) saved (snapped from 1000×130 at (371, 203)) to script egrabberConfig.js and profile lab.");
    expect(describeSavedWindow(reply({ snapped: false, profile: { ...reply().profile, config_updated: false, memory_updated: false, config_has_window: false, reason: "the applied configuration has no camera.window" } })).text)
      .toBe("Camera window 992×128 at (368, 200) saved to script egrabberConfig.js.");
  });

  it("warns when the script lacks a line, or the profile file could not follow", () => {
    const missing = describeSavedWindow(reply({ snapped: false, script: { ...reply().script, missing: ["OffsetX", "OffsetY"] } }));
    expect(missing.level).toBe("warn");
    expect(missing.text).toContain("has no OffsetX/OffsetY line, so that is not saved there");
    const stale = describeSavedWindow(reply({ snapped: false, profile: { ...reply().profile, config_updated: false, reason: "the profile changed on disk since it was applied; apply it again to keep its window" } }));
    expect(stale.level).toBe("warn");
    expect(stale.text).toContain("profile file not changed: the profile changed on disk");
    const noLines = describeSavedWindow(reply({ snapped: false, script: { path: "/x/egrabberConfig.js", updated: false, changed: false, missing: ["Width", "Height", "OffsetX", "OffsetY"] } }));
    expect(noLines.text).toContain("has no Width/Height/OffsetX/OffsetY line to change");
  });

  it("is an error line when the backend refuses", () => {
    expect(describeSavedWindow(reply({ ok: false, error: "Stop the experiment before editing the camera window" })))
      .toEqual({ level: "error", text: "Camera window not saved: Stop the experiment before editing the camera window" });
  });
});

describe("the eGrabber box on the Overview", () => {
  beforeEach(() => { geometryRef.current = egrabber(); scriptPath = "C:\\MIBTest\\include\\egrabberConfig.js"; saveCameraWindow.mockReset(); saveCameraWindow.mockResolvedValue(reply()); vi.useFakeTimers({ shouldAdvanceTime: true }); });
  afterEach(() => { vi.useRealTimers(); });

  it("drags with Qt's snap-down and saves on release into the Experiment script", async () => {
    await act(async () => root.render(<Harness instrumentModes={false} />));
    act(() => api.showDefault(egrabber()));
    expect(api.window).toEqual({ x: 368, y: 616, width: 1216, height: 208 });
    act(() => api.onPointerDown(fakeEvent(400, 650)));
    act(() => api.onPointerMove(fakeEvent(403.6, 653.7)));   // moved by (3.6, 3.7): 371.6 -> round 372 -> down to 368; 619.7 -> 620
    expect(saveCameraWindow).not.toHaveBeenCalled();          // nothing is sent while dragging
    await act(async () => api.onPointerUp());
    expect(saveCameraWindow).toHaveBeenCalledWith("C:\\MIBTest\\include\\egrabberConfig.js", 368, 620, 1216, 208);
    expect(saveCameraRoi).not.toHaveBeenCalled();
    expect(api.window).toEqual({ x: 368, y: 200, width: 992, height: 128 }); // as the backend snapped and saved it
    expect(api.notice?.level).toBe("info");
    expect(log.join(" ")).toContain("Camera window 992×128 at (368, 200) saved");
    expect(refreshed).toBe(1);
  });

  it("saves a W/H edit at once after a short pause, snapped down with the offset pulled back onto the sensor", async () => {
    await act(async () => root.render(<Harness instrumentModes={false} />));
    act(() => api.showDefault(egrabber()));
    act(() => api.edit("width", 1000));
    act(() => api.edit("height", 130));
    expect(api.window).toEqual({ x: 368, y: 616, width: 1000, height: 130 }); // typed value shown as typed
    expect(saveCameraWindow).not.toHaveBeenCalled();
    await act(async () => { await vi.advanceTimersByTimeAsync(WINDOW_SAVE_DELAY_MS + 50); });
    expect(saveCameraWindow).toHaveBeenCalledTimes(1);                          // one save for the burst of edits
    expect(saveCameraWindow).toHaveBeenCalledWith(expect.any(String), 368, 616, 992, 128);
  });

  it("saves immediately when the box loses focus", async () => {
    await act(async () => root.render(<Harness instrumentModes={false} />));
    act(() => api.showDefault(egrabber()));
    act(() => api.edit("width", 640));
    await act(async () => api.flush());
    expect(saveCameraWindow).toHaveBeenCalledWith(expect.any(String), 368, 616, 640, 208);
    await act(async () => api.flush());                                         // nothing pending: no second save
    expect(saveCameraWindow).toHaveBeenCalledTimes(1);
  });

  it("shows a refused save as an error and keeps the box where the operator put it", async () => {
    saveCameraWindow.mockResolvedValue(reply({ ok: false, error: "Experiment camera script file not found: C:\\gone.js" }));
    await act(async () => root.render(<Harness instrumentModes={false} />));
    act(() => api.showDefault(egrabber()));
    act(() => api.edit("x", 400));
    await act(async () => api.flush());
    expect(api.notice).toEqual({ level: "error", text: "Camera window not saved: Experiment camera script file not found: C:\\gone.js" });
    expect(log.join(" ")).toContain("Camera window not saved");
    expect(api.window?.x).toBe(400);
    saveCameraWindow.mockRejectedValue(new Error("transport closed"));
    act(() => api.edit("x", 416));
    await act(async () => api.flush());
    expect(api.notice?.text).toContain("transport closed");
  });

  it("does nothing with the box during an experiment", async () => {
    await act(async () => root.render(<Harness instrumentModes={false} expActive />));
    act(() => api.showDefault(egrabber()));
    act(() => api.edit("width", 640));
    await act(async () => api.flush());
    expect(saveCameraWindow).not.toHaveBeenCalled();
    expect(api.window?.width).toBe(1216);
  });

  it("redraws the box from the backend's window when a profile is applied, but not over an edit or an older reply", async () => {
    await act(async () => root.render(<Harness instrumentModes={false} />));
    act(() => api.showDefault(egrabber()));
    const gen = api.saveGeneration();
    // a reloaded profile with another window
    act(() => api.syncFromGeometry(egrabber({ roi: { x: 16, y: 20, width: 512, height: 96 } }), gen));
    expect(api.window).toEqual({ x: 16, y: 20, width: 512, height: 96 });
    // the same window again changes nothing, and the operator's later drag is kept
    act(() => api.edit("x", 64));
    act(() => api.syncFromGeometry(egrabber({ roi: { x: 32, y: 20, width: 512, height: 96 } }), gen));
    expect(api.window?.x).toBe(64);                                             // an edit in progress is not overwritten
    await act(async () => api.flush());
    // a reply fetched before a save finished is stale
    act(() => api.syncFromGeometry(egrabber({ roi: { x: 800, y: 20, width: 512, height: 96 } }), gen));
    expect(api.window?.x).toBe(368);                                            // the saved window (992x128 at 368,200 -> x 368)
    // no saved window yet: the geometry leaves the default alone
    act(() => api.syncFromGeometry(egrabber({ roi: { x: 0, y: 0, width: 0, height: 0 }, window_source: "none" }), api.saveGeneration()));
    expect(api.window?.x).toBe(368);
  });

  it("keeps the Aravis / MindVision behaviour: the explicit save writes the ROI and edits do not auto-save", async () => {
    geometryRef.current = geometry({ camera: "mindvision", roi: { x: 100, y: 120, width: 256, height: 128 } } as Partial<CameraGeometry>);
    await act(async () => root.render(<Harness instrumentModes={false} />));
    act(() => api.showDefault(geometryRef.current!));
    act(() => api.edit("width", 300));
    await act(async () => { await vi.advanceTimersByTimeAsync(WINDOW_SAVE_DELAY_MS + 50); });
    expect(saveCameraRoi).not.toHaveBeenCalled();
    expect(saveCameraWindow).not.toHaveBeenCalled();
    await act(async () => { await api.save(api.windowRef.current!); });
    expect(saveCameraRoi).toHaveBeenCalledTimes(1);
  });
});
