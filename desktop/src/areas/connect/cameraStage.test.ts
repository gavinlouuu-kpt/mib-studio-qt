import { describe, expect, it, vi } from "vitest";
import type { CameraGeometry, CameraSelection } from "../../bridge";
import {
  experimentWindowNotice, folderOf, joinPath, NO_EXPERIMENT_SCRIPT, NO_OVERVIEW_SCRIPT, NO_SOURCES, overviewAutoStart,
  profileFile, resolveExperimentScript, resolveOverviewScript, runEgrabberStage, runOverviewAutoStart, START_FAILED,
  type ScriptSources, type StageIo,
} from "./cameraStage";

const sources = (over: Partial<ScriptSources> = {}): ScriptSources => ({ ...NO_SOURCES, ...over });
const present = (...paths: string[]) => async (path: string) => paths.includes(path);

describe("script paths (#846)", () => {
  it("finds a profile's files next to its config.json, with the path's own separators", () => {
    expect(folderOf("/data/profiles/lab/config.json")).toBe("/data/profiles/lab");
    expect(folderOf("C:\\MIBTest\\include\\config.json")).toBe("C:\\MIBTest\\include");
    expect(folderOf("config.json")).toBe("");
    expect(joinPath("C:\\MIBTest\\include", "overviewConfig.js")).toBe("C:\\MIBTest\\include\\overviewConfig.js");
    expect(joinPath("/a/b/", "c.js")).toBe("/a/b/c.js");
    expect(profileFile("/data/profiles/lab/config.json", "egrabberConfig.js")).toBe("/data/profiles/lab/egrabberConfig.js");
    expect(profileFile("", "egrabberConfig.js")).toBe("");
  });

  it("takes the Experiment script from the Config selection, else the editor's file, else the profile's egrabberConfig.js", async () => {
    const all = sources({ cameraScriptPath: "/sel.js", cameraDocumentPath: "/doc.js", profileConfigPath: "/p/lab/config.json" });
    expect(await resolveExperimentScript(all, present())).toEqual({ path: "/sel.js", source: "selection" });
    expect(await resolveExperimentScript({ ...all, cameraScriptPath: "" }, present())).toEqual({ path: "/doc.js", source: "document" });
    expect(await resolveExperimentScript({ ...all, cameraScriptPath: "", cameraDocumentPath: "" }, present("/p/lab/egrabberConfig.js")))
      .toEqual({ path: "/p/lab/egrabberConfig.js", source: "profile" });
    // the profile's script only counts when the file is there
    expect(await resolveExperimentScript({ ...all, cameraScriptPath: "", cameraDocumentPath: "" }, present())).toBeNull();
    expect(await resolveExperimentScript(NO_SOURCES, present())).toBeNull();
  });

  it("takes the Overview script from the editor's path, else the profile's overviewConfig.js when it exists", async () => {
    const profile = sources({ profileConfigPath: "C:\\MIBTest\\include\\config.json" });
    expect(await resolveOverviewScript(profile, present("C:\\MIBTest\\include\\overviewConfig.js")))
      .toEqual({ path: "C:\\MIBTest\\include\\overviewConfig.js", source: "profile" });
    expect(await resolveOverviewScript(profile, present())).toBeNull();
    expect(await resolveOverviewScript({ ...profile, overviewScriptPath: "/x/overviewConfig.js" }, present()))
      .toEqual({ path: "/x/overviewConfig.js", source: "overview-editor" });
  });
});

const selection = (over: Partial<CameraSelection> = {}): CameraSelection => ({
  valid: true, configured: true, mode: 2, running: false, interface_index: 0, device_index: 0, label: "MC2066",
  mindvision_index: -1, mindvision_config_path: "", camera_script_path: "", mock_frame_dir: "", mock_interval_ms: 0, mock_loop: false, ...over,
});
const geometry = (over: Partial<CameraGeometry> = {}): CameraGeometry => ({
  supported: true, overview: false, camera: "egrabber", sensor_width: 1920, sensor_height: 1080, roi: { x: 368, y: 616, width: 1216, height: 208 },
  width_increment: 16, height_increment: 4, offset_x_increment: 16, offset_y_increment: 4, min_width: 64, min_height: 16, session: {},
  window_source: "profile", script_role: "experiment", experiment_script_path: "", window_apply: null, ...over,
} as CameraGeometry);

/** A fake backend that records the calls in order. */
function fake(opts: { selection?: Partial<CameraSelection>; apply?: { ok: boolean; message: string }; start?: { ok: boolean; message: string };
  stop?: { ok: boolean; message: string }; geometry?: CameraGeometry | null; existing?: string[] } = {}) {
  const calls: string[] = [];
  let sel = selection(opts.selection);
  const io: StageIo = {
    fetchSelection: async () => { calls.push("selection"); return sel; },
    stopCapture: async () => { calls.push("stop"); const r = opts.stop ?? { ok: true, message: "Capture stopped" }; if (r.ok) sel = { ...sel, running: false }; return r; },
    startCapture: async () => { calls.push("start"); const r = opts.start ?? { ok: true, message: "Capture started" }; if (r.ok) sel = { ...sel, running: true }; return r; },
    applyScript: async (path, overview) => { calls.push(`apply ${overview ? "overview" : "experiment"} ${path}`); return opts.apply ?? { ok: true, message: "Camera script applied" }; },
    refreshGeometry: async () => { calls.push("geometry"); return opts.geometry === undefined ? geometry() : opts.geometry; },
    scriptExists: async (path) => (opts.existing ?? []).includes(path),
    setRunning: (running) => { calls.push(`running ${running}`); },
    log: () => undefined,
  };
  return { io, calls };
}
const input = (over: Record<string, unknown> = {}) => ({
  stage: "overview" as const, expActive: false, recording: false, geometry: geometry(),
  sources: sources({ cameraScriptPath: "/exp/egrabberConfig.js", profileConfigPath: "/p/lab/config.json" }), ...over,
});

describe("entering Overview with an eGrabber camera (#846)", () => {
  it("stops a running camera, applies the Overview script with the Overview role, and restarts it", async () => {
    const { io, calls } = fake({ selection: { running: true }, existing: ["/p/lab/overviewConfig.js"] });
    const notice = await runEgrabberStage(input(), io);
    expect(calls).toEqual(["selection", "stop", "running false", "apply overview /p/lab/overviewConfig.js", "geometry", "start", "running true"]);
    expect(notice).toBeNull();
  });

  it("starts a stopped, configured camera after the script (Qt auto-start)", async () => {
    const { io, calls } = fake({ existing: ["/p/lab/overviewConfig.js"] });
    expect(await runEgrabberStage(input(), io)).toBeNull();
    expect(calls).toEqual(["selection", "apply overview /p/lab/overviewConfig.js", "geometry", "start", "running true"]);
  });

  it("says so when the auto-start fails, with Qt's sentence", async () => {
    const { io } = fake({ existing: ["/p/lab/overviewConfig.js"], start: { ok: false, message: "device busy" } });
    const notice = await runEgrabberStage(input(), io);
    expect(notice).toEqual({ stage: "overview", level: "error", text: START_FAILED });
    expect(START_FAILED).toBe("Failed to start camera. Please check camera connection and try again.");
  });

  it("says when a running camera was not restarted after the script", async () => {
    const { io } = fake({ selection: { running: true }, existing: ["/p/lab/overviewConfig.js"], start: { ok: false, message: "device busy" } });
    const notice = await runEgrabberStage(input(), io);
    expect(notice?.level).toBe("error");
    expect(notice?.text).toBe("Camera script applied, but restart failed: device busy");
  });

  it("with no Overview script applies nothing but still starts a stopped camera", async () => {
    const { io, calls } = fake();
    const notice = await runEgrabberStage(input(), io);
    expect(calls).toEqual(["selection", "start", "running true"]);
    expect(notice).toEqual({ stage: "overview", level: "warn", text: NO_OVERVIEW_SCRIPT });
    // a running camera is left alone
    const running = fake({ selection: { running: true } });
    expect((await runEgrabberStage(input(), running.io))?.text).toBe(NO_OVERVIEW_SCRIPT);
    expect(running.calls).toEqual(["selection"]);
  });

  it("leaves the capture stopped and says why when the script is refused", async () => {
    const { io, calls } = fake({ selection: { running: true }, existing: ["/p/lab/overviewConfig.js"], apply: { ok: false, message: "script error at line 3" } });
    const notice = await runEgrabberStage(input(), io);
    expect(calls).toEqual(["selection", "stop", "running false", "apply overview /p/lab/overviewConfig.js", "geometry"]);
    expect(notice).toEqual({ stage: "overview", level: "error", text: "Overview camera script not applied: script error at line 3" });
  });

  it("does not touch the camera when it cannot be stopped", async () => {
    const { io, calls } = fake({ selection: { running: true }, existing: ["/p/lab/overviewConfig.js"], stop: { ok: false, message: "busy" } });
    const notice = await runEgrabberStage(input(), io);
    expect(calls).toEqual(["selection", "stop"]);
    expect(notice?.text).toContain("Camera not stopped");
  });
});

describe("entering Experiment with an eGrabber camera (#846)", () => {
  const experiment = (over: Record<string, unknown> = {}) => input({ stage: "experiment", ...over });

  it("applies the Experiment script with the Experiment role and restarts a running camera, without auto-starting a stopped one", async () => {
    const running = fake({ selection: { running: true } });
    expect(await runEgrabberStage(experiment(), running.io)).toBeNull();
    expect(running.calls).toEqual(["selection", "stop", "running false", "apply experiment /exp/egrabberConfig.js", "geometry", "start", "running true"]);
    const stopped = fake();
    await runEgrabberStage(experiment(), stopped.io);
    expect(stopped.calls).toEqual(["selection", "apply experiment /exp/egrabberConfig.js", "geometry"]);
  });

  it("shows the window the camera ended up with, the profile's when it won", async () => {
    const apply = {
      sequence: 2, requested: true, applied: true, script_window: { x: 704, y: 500, width: 512, height: 96 },
      profile_window: { x: 368, y: 616, width: 1216, height: 208 }, result_window: { x: 368, y: 616, width: 1216, height: 208 },
      camera: "SVS-Vistek GmbH MC2066", refusal: "", error: "",
      text: "the profile's camera window 1216x208 at (368, 616) is applied instead of the script's 512x96 at (704, 500)",
    };
    const { io } = fake({ geometry: geometry({ window_apply: apply }) });
    const notice = await runEgrabberStage(experiment(), io);
    expect(notice?.level).toBe("info");
    expect(notice?.text).toContain("1216x208 at (368, 616) is applied instead");
    // a refused or rejected profile window is an error line
    const refused = experimentWindowNotice(geometry({ window_apply: { ...apply, applied: false, refusal: "made for another camera", text: "the profile's camera window 1216x208 at (368, 616) was not applied: made for another camera" } }));
    expect(refused?.level).toBe("error");
    expect(refused?.text).toContain("made for another camera");
    expect(experimentWindowNotice(geometry({ window_apply: null }))).toBeNull();
  });

  it("with no Experiment script applies nothing and says the camera keeps its window", async () => {
    const { io, calls } = fake({ selection: { running: true } });
    const notice = await runEgrabberStage(experiment({ sources: NO_SOURCES }), io);
    expect(calls).toEqual(["selection"]);
    expect(notice).toEqual({ stage: "experiment", level: "warn", text: NO_EXPERIMENT_SCRIPT });
    expect(NO_EXPERIMENT_SCRIPT).toBe("No Experiment camera script: the camera keeps its current window");
  });

  it("falls back to the profile's egrabberConfig.js when nothing is chosen", async () => {
    const { io, calls } = fake({ existing: ["/p/lab/egrabberConfig.js"] });
    await runEgrabberStage(experiment({ sources: sources({ profileConfigPath: "/p/lab/config.json" }) }), io);
    expect(calls).toContain("apply experiment /p/lab/egrabberConfig.js");
  });
});

describe("never while something else owns the camera", () => {
  it("does nothing during an experiment, with raw recording on (it says so), or without a configured hardware camera", async () => {
    for (const stage of ["overview", "experiment"] as const) {
      const exp = fake({ selection: { running: true } });
      expect(await runEgrabberStage(input({ stage, expActive: true }), exp.io)).toBeNull();
      expect(exp.calls).toEqual([]);
      const rec = fake({ selection: { running: true } });
      expect((await runEgrabberStage(input({ stage, recording: true }), rec.io))?.text).toContain("Stop the raw recording");
      expect(rec.calls).toEqual([]);
    }
    for (const sel of [{ configured: false }, { mode: 1 }, { valid: false }]) {
      const none = fake({ selection: sel });
      expect(await runEgrabberStage(input(), none.io)).toBeNull();
      expect(none.calls).toEqual(["selection"]);
    }
  });

  it("reports a thrown backend error instead of throwing", async () => {
    const { io } = fake({ existing: ["/p/lab/overviewConfig.js"] });
    io.applyScript = async () => { throw new Error("transport closed"); };
    const notice = await runEgrabberStage(input(), io);
    expect(notice?.level).toBe("error");
    expect(notice?.text).toContain("transport closed");
  });
});

describe("Overview auto-start for the other cameras (#863 CONNECT-042)", () => {
  const sel = (over: Partial<Pick<CameraSelection, "configured" | "running">> = {}) => ({ configured: true, running: false, ...over });
  it("starts a stopped, configured camera, never an illuminated or unreadable MindVision profile, never on the PZ7035", () => {
    expect(overviewAutoStart(null, sel(), false)).toBe(true);
    expect(overviewAutoStart(geometry({ camera: "mindvision", illuminated_live: false, profile_error: "" }), sel(), false)).toBe(true);
    expect(overviewAutoStart(geometry({ camera: "mindvision", illuminated_live: true }), sel(), false)).toBe(false);
    expect(overviewAutoStart(geometry({ camera: "mindvision", profile_error: "Cannot read MindVision experiment profile" }), sel(), false)).toBe(false);
    expect(overviewAutoStart(null, sel({ running: true }), false)).toBe(false);
    expect(overviewAutoStart(null, sel({ configured: false }), false)).toBe(false);
    expect(overviewAutoStart(null, sel(), true)).toBe(false);
  });

  it("starts through the shared start path and reports a failure with Qt's sentence", async () => {
    const start = vi.fn(async () => ({ ok: true, message: "" }));
    const io = { fetchSelection: async () => selection({ mode: 1 }), startCapture: start, setRunning: vi.fn(), log: vi.fn() };
    expect(await runOverviewAutoStart({ expActive: false, geometry: null, instrumentModes: false }, io)).toBeNull();
    expect(start).toHaveBeenCalledOnce();
    expect(io.setRunning).toHaveBeenCalledWith(true);
    const failing = { ...io, startCapture: async () => ({ ok: false, message: "no device" }) };
    expect(await runOverviewAutoStart({ expActive: false, geometry: null, instrumentModes: false }, failing))
      .toEqual({ stage: "overview", level: "error", text: START_FAILED });
    start.mockClear();
    expect(await runOverviewAutoStart({ expActive: true, geometry: null, instrumentModes: false }, io)).toBeNull();
    expect(start).not.toHaveBeenCalled();
  });
});
