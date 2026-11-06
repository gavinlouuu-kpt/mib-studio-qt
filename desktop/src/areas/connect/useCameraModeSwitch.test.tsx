// @vitest-environment jsdom
// #846: entering Overview / Experiment with an eGrabber camera applies the stage's script with its role
// (the early return on `supported=false` is gone), once per entry, never during an experiment; the other
// cameras keep their preset switch and Overview starts a stopped, configured camera.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { CameraGeometry, CameraSelection } from "../../bridge";
import { NO_SOURCES, type ScriptSources, type StageNotice } from "./cameraStage";

const bridgeMock = vi.hoisted(() => ({
  fetchCameraSelection: vi.fn(), stopCapture: vi.fn(), startCapture: vi.fn(), applyCameraScriptWithRole: vi.fn(),
  setCameraOverview: vi.fn(), setInstrumentMode: vi.fn(),
}));
vi.mock("../../bridge", () => ({ bridge: bridgeMock }));
const invokeMock = vi.hoisted(() => vi.fn());
// A browser session is "viewer only" when another client holds control (transport/useControlRole.ts).
const sessionState = vi.hoisted(() => ({ current: {} as { clientId?: string; controllerId?: string | null } }));
vi.mock("../../transport", () => ({
  invoke: invokeMock,
  transport: { session: { subscribe: () => () => undefined, get: () => sessionState.current } },
}));
import { syncScriptSources, useCameraModeSwitch } from "./useConnect";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

const selection = (over: Partial<CameraSelection> = {}): CameraSelection => ({
  valid: true, configured: true, mode: 2, running: false, interface_index: 0, device_index: 0, label: "MC2066",
  mindvision_index: -1, mindvision_config_path: "", camera_script_path: "", mock_frame_dir: "", mock_interval_ms: 0, mock_loop: false, ...over,
});
const geometry = (over: Partial<CameraGeometry> = {}): CameraGeometry => ({
  supported: true, overview: false, camera: "egrabber", sensor_width: 1920, sensor_height: 1080,
  roi: { x: 368, y: 616, width: 1216, height: 208 }, width_increment: 16, height_increment: 4, offset_x_increment: 16,
  offset_y_increment: 4, min_width: 64, min_height: 16, session: {}, window_source: "profile", ...over,
} as CameraGeometry);

let host: HTMLDivElement, root: Root;
let current: CameraGeometry | null, sel: CameraSelection, notices: Array<StageNotice | null>, running: boolean[], exists: Set<string>, created: string[];
let sources: { current: ScriptSources };
let tabRef: { current: string };
// Start / Stop Camera's one-command guard (B's cameraCommandRef) and what the stage change did to it.
let commandRef: { current: string | null }, commandSeen: Array<string | null>;

type Props = { tab: "connect" | "overview" | "experiment" | "review"; expActive?: boolean; recording?: boolean; ready?: boolean };
function Harness({ tab, expActive = false, recording = false, ready = true }: Props) {
  tabRef.current = tab;
  useCameraModeSwitch({
    ready, tab, tabRef: tabRef as never, append: () => undefined, instrumentModes: false, instrumentRef: { current: undefined } as never,
    instrumentMode: undefined, instrument: null, runMode: false, cameraWindowRef: { current: null }, setModeError: () => undefined,
    modeRetry: 0, modeError: null, setRunning: (r: boolean) => running.push(r), expActive, recording, setReviewing: () => undefined,
    cameraWin: { placed: false, placedRef: { current: false }, restore: () => undefined } as never,
    refreshCameraGeometry: STABLE_REFRESH, scriptSourcesRef: sources, setStageNotice: (n: StageNotice | null) => notices.push(n),
    cameraCommandRef: commandRef, setCameraCommand: (c: string | null) => commandSeen.push(c),
  } as never);
  return null;
}
const STABLE_REFRESH = async () => current;

beforeEach(() => {
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
  current = geometry(); sel = selection(); notices = []; running = []; created = [];
  exists = new Set(["/p/lab/overviewConfig.js", "/p/lab/egrabberConfig.js"]);
  sources = { current: { ...NO_SOURCES, profileConfigPath: "/p/lab/config.json" } };
  tabRef = { current: "connect" };
  commandRef = { current: null }; commandSeen = [];
  sessionState.current = {};
  for (const fn of Object.values(bridgeMock)) fn.mockReset();
  bridgeMock.fetchCameraSelection.mockImplementation(async () => sel);
  bridgeMock.stopCapture.mockImplementation(async () => { sel = { ...sel, running: false }; return { ok: true, message: "stopped" }; });
  bridgeMock.startCapture.mockImplementation(async () => { sel = { ...sel, running: true }; return { ok: true, message: "started" }; });
  bridgeMock.applyCameraScriptWithRole.mockResolvedValue({ ok: true, message: "applied" });
  bridgeMock.setCameraOverview.mockResolvedValue({ ok: true, message: "mode" });
  invokeMock.mockImplementation(async (_command: string, args: { action: string; path: string; kind: string; text: string }) => {
    if (args.action === "default") return { path: "", text: "// bundled overviewConfig.js", revision: "" };
    if (args.action === "create") { exists.add(args.path); created.push(`${args.kind} ${args.path}: ${args.text}`); return { path: args.path, text: args.text, revision: "r" }; }
    if (!exists.has(args.path)) throw new Error("not found");
    return { path: args.path, text: "", revision: "r" };
  });
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

const render = async (props: Props) => { await act(async () => { root.render(<Harness {...props} />); }); await act(async () => { await Promise.resolve(); }); };

describe("entering a stage with an eGrabber camera", () => {
  it("applies the Overview script with the Overview role and starts the stopped camera", async () => {
    await render({ tab: "overview" });
    expect(bridgeMock.applyCameraScriptWithRole).toHaveBeenCalledTimes(1);
    expect(bridgeMock.applyCameraScriptWithRole).toHaveBeenCalledWith("/p/lab/overviewConfig.js", true);
    expect(bridgeMock.startCapture).toHaveBeenCalledTimes(1);
    expect(bridgeMock.setCameraOverview).not.toHaveBeenCalled(); // no preset for this camera
    expect(notices[notices.length - 1]).toBeNull();
  });

  it("applies the Overview editor's file (#863 A2): the external script the operator browsed to, not the profile's", async () => {
    exists.add("/usb/overview.js");
    sources.current = { ...sources.current, overviewScriptPath: "/usb/overview.js", overviewDefaultPath: "/p/lab/overviewConfig.js" };
    await render({ tab: "overview" });
    expect(bridgeMock.applyCameraScriptWithRole).toHaveBeenCalledWith("/usb/overview.js", true);
    expect(created).toEqual([]);
  });

  it("makes the default Overview script from the bundled one when its file is missing, then applies it (Qt ensureDefaultsFile)", async () => {
    exists.delete("/p/lab/overviewConfig.js");
    sources.current = { ...sources.current, overviewScriptPath: "/p/lab/overviewConfig.js", overviewDefaultPath: "/p/lab/overviewConfig.js" };
    await render({ tab: "overview" });
    expect(created).toEqual(["overview /p/lab/overviewConfig.js: // bundled overviewConfig.js"]);
    expect(bridgeMock.applyCameraScriptWithRole).toHaveBeenCalledWith("/p/lab/overviewConfig.js", true);
  });

  it("applies nothing and says so when the external Overview script is gone, and still starts a stopped camera", async () => {
    sources.current = { ...sources.current, overviewScriptPath: "/usb/gone.js", overviewDefaultPath: "/p/lab/overviewConfig.js" };
    await render({ tab: "overview" });
    expect(bridgeMock.applyCameraScriptWithRole).not.toHaveBeenCalled();
    expect(created).toEqual([]);
    expect(bridgeMock.startCapture).toHaveBeenCalledTimes(1);
    expect(notices[notices.length - 1]).toEqual({ stage: "overview", level: "warn", text: "Overview camera script not found: /usb/gone.js. The camera keeps its current mode." });
  });

  it("applies once per entry, not on every re-render, and again after leaving and coming back", async () => {
    await render({ tab: "overview" });
    await render({ tab: "overview", recording: false });
    await render({ tab: "overview", expActive: false, ready: true });
    expect(bridgeMock.applyCameraScriptWithRole).toHaveBeenCalledTimes(1);
    await render({ tab: "experiment" });
    expect(bridgeMock.applyCameraScriptWithRole).toHaveBeenLastCalledWith("/p/lab/egrabberConfig.js", false);
    expect(bridgeMock.applyCameraScriptWithRole).toHaveBeenCalledTimes(2);
    await render({ tab: "review" });
    await render({ tab: "experiment" });
    expect(bridgeMock.applyCameraScriptWithRole).toHaveBeenCalledTimes(3);
  });

  it("stops the running camera, applies the Experiment script and restarts it", async () => {
    sel = selection({ running: true });
    await render({ tab: "experiment" });
    expect(bridgeMock.stopCapture).toHaveBeenCalledTimes(1);
    expect(bridgeMock.applyCameraScriptWithRole).toHaveBeenCalledWith("/p/lab/egrabberConfig.js", false);
    expect(bridgeMock.startCapture).toHaveBeenCalledTimes(1);
    expect(bridgeMock.stopCapture.mock.invocationCallOrder[0]).toBeLessThan(bridgeMock.applyCameraScriptWithRole.mock.invocationCallOrder[0]);
    expect(bridgeMock.applyCameraScriptWithRole.mock.invocationCallOrder[0]).toBeLessThan(bridgeMock.startCapture.mock.invocationCallOrder[0]);
    expect(running).toEqual([false, true]);
  });

  it("does nothing during an experiment, even when the stage is entered", async () => {
    await render({ tab: "experiment", expActive: true });
    await render({ tab: "experiment", expActive: false });  // the entry was consumed
    expect(bridgeMock.applyCameraScriptWithRole).not.toHaveBeenCalled();
    expect(bridgeMock.stopCapture).not.toHaveBeenCalled();
  });

  it("says when there is no Experiment script instead of applying anything", async () => {
    exists.clear();
    await render({ tab: "experiment" });
    expect(bridgeMock.applyCameraScriptWithRole).not.toHaveBeenCalled();
    expect(notices[notices.length - 1]).toEqual({ stage: "experiment", level: "warn", text: "No Experiment camera script: the camera keeps its current window" });
  });

  it("is navigation only for a browser that does not control the instrument", async () => {
    sessionState.current = { clientId: "viewer", controllerId: "operator" };
    await render({ tab: "overview" });
    await render({ tab: "experiment" });
    expect(bridgeMock.applyCameraScriptWithRole).not.toHaveBeenCalled();
    expect(bridgeMock.stopCapture).not.toHaveBeenCalled();
    expect(bridgeMock.startCapture).not.toHaveBeenCalled();
    expect(bridgeMock.setCameraOverview).not.toHaveBeenCalled();
    expect(notices).toEqual([]);
  });

  it("leaves the camera alone on the Connect and Review stages", async () => {
    await render({ tab: "connect" });
    await render({ tab: "review" });
    expect(bridgeMock.fetchCameraSelection).not.toHaveBeenCalled();
    expect(bridgeMock.applyCameraScriptWithRole).not.toHaveBeenCalled();
  });
});

// #863 integration: the stage change shares Start / Stop Camera's one-command guard (B's CONNECT-033).
describe("the stage change and Start / Stop Camera's one-command guard", () => {
  it("holds the guard from the camera stop to the restart, so no Start can slip in while the script is applied", async () => {
    sel = selection({ running: true });
    const heldAt: Record<string, string | null> = {};
    bridgeMock.stopCapture.mockImplementation(async () => { heldAt.stop = commandRef.current; sel = { ...sel, running: false }; return { ok: true, message: "stopped" }; });
    bridgeMock.applyCameraScriptWithRole.mockImplementation(async () => { heldAt.apply = commandRef.current; return { ok: true, message: "applied" }; });
    bridgeMock.startCapture.mockImplementation(async () => { heldAt.start = commandRef.current; sel = { ...sel, running: true }; return { ok: true, message: "started" }; });
    await render({ tab: "experiment" });
    expect(heldAt).toEqual({ stop: "stage", apply: "stage", start: "stage" });
    expect(commandRef.current).toBeNull();
    expect(commandSeen).toEqual(["stage", null]);
  });

  it("waits for a Start Camera click that is still in flight before it stops and restarts the camera", async () => {
    commandRef.current = "start"; // the click that moved the operator to the Overview is still waiting for its reply
    await render({ tab: "overview" });
    await act(async () => { await new Promise((r) => setTimeout(r, 120)); });
    expect(bridgeMock.applyCameraScriptWithRole).not.toHaveBeenCalled();
    expect(bridgeMock.startCapture).not.toHaveBeenCalled();
    commandRef.current = null; // the Start finished
    await act(async () => { await new Promise((r) => setTimeout(r, 120)); });
    expect(bridgeMock.applyCameraScriptWithRole).toHaveBeenCalledTimes(1);
    expect(bridgeMock.startCapture).toHaveBeenCalledTimes(1);
    expect(commandRef.current).toBeNull();
  });

  it("holds the guard while a camera without scripts auto-starts on Overview, and lets it go when the start fails", async () => {
    current = geometry({ supported: false, camera: "" });
    sel = selection({ mode: 1 });
    let during: string | null = null;
    bridgeMock.startCapture.mockImplementation(async () => { during = commandRef.current; return { ok: false, message: "no frames" }; });
    await render({ tab: "overview" });
    expect(during).toBe("stage");
    expect(commandRef.current).toBeNull();
    expect(notices[notices.length - 1]).toMatchObject({ level: "error" });
  });

  it("does not touch the camera when the operator has left the stage before the stage change gets its turn", async () => {
    commandRef.current = "start";
    await render({ tab: "overview" });
    tabRef.current = "review";
    await render({ tab: "review" });
    commandRef.current = null;
    await act(async () => { await new Promise((r) => setTimeout(r, 120)); });
    expect(bridgeMock.applyCameraScriptWithRole).not.toHaveBeenCalled();
    expect(bridgeMock.startCapture).not.toHaveBeenCalled();
  });
});

describe("the other cameras", () => {
  it("keep the preset switch, and Overview starts a stopped, configured camera once", async () => {
    current = geometry({ camera: "aravis", overview: false });
    sel = selection({ mode: 3 });
    await render({ tab: "overview" });
    expect(bridgeMock.setCameraOverview).toHaveBeenCalledWith(true);
    expect(bridgeMock.applyCameraScriptWithRole).not.toHaveBeenCalled();
    expect(bridgeMock.startCapture).toHaveBeenCalledTimes(1);
    await render({ tab: "overview", recording: true });
    expect(bridgeMock.startCapture).toHaveBeenCalledTimes(1);
  });

  it("never auto-start a MindVision camera, illuminated or not (Qt returns first)", async () => {
    for (const illuminated of [true, false]) {
      current = geometry({ camera: "mindvision", overview: true, illuminated_live: illuminated });
      sel = selection({ mode: 3 });
      await render({ tab: "overview" });
      expect(bridgeMock.startCapture).not.toHaveBeenCalled();
    }
  });

  it("start the mock camera on Overview and say so when it fails", async () => {
    current = geometry({ supported: false, camera: "" });
    sel = selection({ mode: 1 });
    bridgeMock.startCapture.mockResolvedValue({ ok: false, message: "no frames" });
    await render({ tab: "overview" });
    expect(bridgeMock.startCapture).toHaveBeenCalledTimes(1);
    expect(notices[notices.length - 1]).toEqual({ stage: "overview", level: "error", text: "Failed to start camera. Please check camera connection and try again." });
  });
});


describe("where the stage change finds the scripts (#863 A2)", () => {
  it("fills the Overview slots with the editor's resolved path and its default, beside the Config choices", () => {
    const ref = { current: NO_SOURCES };
    syncScriptSources({
      scriptSourcesRef: ref, cameraScript: { path: "/sel.js" }, scriptDocument: { doc: { path: "/doc.js" } }, profiles: { activeProfile: { path: "/p/lab/config.json" } },
      overviewScript: { path: "/usb/overview.js", defaultPath: "/p/lab/overviewConfig.js" },
    } as never);
    expect(ref.current).toEqual({
      cameraScriptPath: "/sel.js", cameraDocumentPath: "/doc.js", profileConfigPath: "/p/lab/config.json",
      overviewScriptPath: "/usb/overview.js", overviewDefaultPath: "/p/lab/overviewConfig.js",
    });
    // and follows the editor each render: the default again once the external path is cleared
    syncScriptSources({
      scriptSourcesRef: ref, cameraScript: { path: "" }, scriptDocument: { doc: null }, profiles: { activeProfile: null },
      overviewScript: { path: "/home/u/.config/app/overviewConfig.js", defaultPath: "/home/u/.config/app/overviewConfig.js" },
    } as never);
    expect(ref.current.overviewScriptPath).toBe("/home/u/.config/app/overviewConfig.js");
    expect(ref.current.profileConfigPath).toBe("");
  });
});
