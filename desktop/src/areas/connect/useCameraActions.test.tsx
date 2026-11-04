// @vitest-environment jsdom
// What surrounds Start / Stop Camera (epic #863, B2): the phase text's lifecycle poll (CONNECT-034), the space bar
// (CONNECT-041), the camera the active profile expects (App.tsx:1237) and the configured focus staleness (App.tsx:1306).
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { bridge, type CaptureLifecycle } from "../../bridge";
import { configDocument } from "../../configDocument";
import { useCameraPhase, useCameraShortcut, useProfileCameraContext } from "./useCameraActions";

vi.mock("../../bridge", () => ({ bridge: { fetchCaptureLifecycle: vi.fn(), fetchAutofocusConfig: vi.fn() } }));
vi.mock("../../configDocument", () => ({ configDocument: { read: vi.fn() } }));

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let host: HTMLDivElement, root: Root;
beforeEach(() => {
  vi.useFakeTimers(); vi.clearAllMocks();
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.useRealTimers(); document.body.innerHTML = ""; });

const lifecycle = (p: Partial<CaptureLifecycle> = {}): CaptureLifecycle => ({
  valid: true, state: "idle", generation: "1", camera_ready: false, failure: "none", message: "", failure_generation: "0", ...p,
});

describe("the capture lifecycle behind the phase text", () => {
  function Phase({ ready, setLifecycle, cameraCommandRef }: Parameters<typeof useCameraPhase>[0]) {
    useCameraPhase({ ready, setLifecycle, cameraCommandRef });
    return null;
  }
  const mount = (props: Parameters<typeof useCameraPhase>[0]) => act(async () => root.render(<Phase {...props} />));
  const tick = (ms: number) => act(async () => { await vi.advanceTimersByTimeAsync(ms); });

  it("reads the lifecycle at once, then every 250 ms; a starting or stopping session is read faster", async () => {
    const seen: Array<CaptureLifecycle | null> = [];
    const setLifecycle = (v: CaptureLifecycle | null) => { seen.push(v); };
    vi.mocked(bridge.fetchCaptureLifecycle).mockResolvedValue(lifecycle());
    await mount({ ready: true, setLifecycle: setLifecycle as never, cameraCommandRef: { current: null } });
    expect(bridge.fetchCaptureLifecycle).toHaveBeenCalledTimes(1);
    await tick(250);
    expect(bridge.fetchCaptureLifecycle).toHaveBeenCalledTimes(2);
    await tick(240);
    expect(bridge.fetchCaptureLifecycle).toHaveBeenCalledTimes(2);
    vi.mocked(bridge.fetchCaptureLifecycle).mockResolvedValue(lifecycle({ state: "starting" }));
    await tick(20);
    expect(bridge.fetchCaptureLifecycle).toHaveBeenCalledTimes(3);
    const before = vi.mocked(bridge.fetchCaptureLifecycle).mock.calls.length;
    await tick(200);
    expect(vi.mocked(bridge.fetchCaptureLifecycle).mock.calls.length - before).toBeGreaterThanOrEqual(3); // ~60 ms while it settles
    expect(seen[seen.length - 1]?.state).toBe("starting");
  });

  it("an invalid reading clears the phase to the running flag; a command in flight is read fast; not ready reads nothing", async () => {
    const setLifecycle = vi.fn();
    vi.mocked(bridge.fetchCaptureLifecycle).mockResolvedValue({ ...lifecycle(), valid: false });
    const ref = { current: "start" as "start" | "stop" | null };
    await mount({ ready: true, setLifecycle, cameraCommandRef: ref as never });
    expect(setLifecycle).toHaveBeenLastCalledWith(null);
    await tick(130);
    expect(vi.mocked(bridge.fetchCaptureLifecycle).mock.calls.length).toBeGreaterThanOrEqual(3);
    await mount({ ready: false, setLifecycle, cameraCommandRef: ref as never });
    const calls = vi.mocked(bridge.fetchCaptureLifecycle).mock.calls.length;
    await tick(1000);
    expect(vi.mocked(bridge.fetchCaptureLifecycle).mock.calls.length).toBe(calls);
    expect(setLifecycle).toHaveBeenLastCalledWith(null);
  });

  it("a failed read keeps polling (the shell reports a lost backend)", async () => {
    vi.mocked(bridge.fetchCaptureLifecycle).mockRejectedValueOnce(new Error("gone")).mockResolvedValue(lifecycle());
    const setLifecycle = vi.fn();
    await mount({ ready: true, setLifecycle, cameraCommandRef: { current: null } });
    await tick(260);
    expect(setLifecycle).toHaveBeenCalledWith(expect.objectContaining({ state: "idle" }));
  });
});

describe("the space bar", () => {
  type Deps = Parameters<typeof useCameraShortcut>[0];
  let deps: Deps;
  const calls = { start: vi.fn(), stop: vi.fn(), refuse: vi.fn() };
  function Shortcut() { useCameraShortcut(deps); return <><input data-testid="field" /><button data-testid="btn">b</button></>; }
  const base = (): Deps => ({
    ready: true, tab: "overview", expActive: false, recording: false, running: false, cameraCommand: null, startCameraReason: undefined,
    onStartCamera: calls.start as never, onStopCamera: calls.stop as never, refuse: calls.refuse as never,
  });
  const press = (target: Element | Window = document.body, init: KeyboardEventInit = {}) => {
    const e = new KeyboardEvent("keydown", { key: " ", code: "Space", bubbles: true, cancelable: true, ...init });
    (target as EventTarget).dispatchEvent(e);
    return e;
  };
  beforeEach(async () => {
    deps = base();
    await act(async () => root.render(<Shortcut />));
  });
  const rerender = async (over: Partial<Deps>) => { deps = { ...deps, ...over }; await act(async () => root.render(<Shortcut />)); };

  it("starts a stopped camera and stops a running one through the guarded commands", async () => {
    const e = press();
    expect(calls.start).toHaveBeenCalledOnce();
    expect(e.defaultPrevented).toBe(true);
    await rerender({ running: true });
    press();
    expect(calls.stop).toHaveBeenCalledOnce();
    expect(calls.refuse).not.toHaveBeenCalled();
  });

  it("a Start that is not allowed is the warning with Qt's reason, and nothing is started", async () => {
    await rerender({ startCameraReason: "No camera is configured. Please connect to a camera or configure a mock camera first." });
    press();
    expect(calls.start).not.toHaveBeenCalled();
    expect(calls.refuse).toHaveBeenCalledWith("Camera", "No camera is configured. Please connect to a camera or configure a mock camera first.");
  });

  it("never ends a raw recording: Stop is refused with Qt's text (the Stop Camera button finishes it)", async () => {
    await rerender({ running: true, recording: true });
    press();
    expect(calls.stop).not.toHaveBeenCalled();
    expect(calls.refuse).toHaveBeenCalledWith("Camera", "Cannot stop camera while frame recording is active. Stop recording first.");
  });

  it("a second press while a command is in flight does nothing, silently", async () => {
    await rerender({ cameraCommand: "start", startCameraReason: "A camera command is already in progress." });
    press();
    expect(calls.start).not.toHaveBeenCalled();
    expect(calls.refuse).not.toHaveBeenCalled();
  });

  it("never during an experiment, on the Connect and Review stages, or before the backend is ready", async () => {
    await rerender({ expActive: true }); press();
    await rerender({ expActive: false, tab: "connect" }); press();
    await rerender({ tab: "review" }); press();
    await rerender({ tab: "overview", ready: false }); press();
    expect(calls.start).not.toHaveBeenCalled();
    expect(calls.stop).not.toHaveBeenCalled();
    expect(calls.refuse).not.toHaveBeenCalled();
    await rerender({ tab: "experiment", ready: true }); press();
    expect(calls.start).toHaveBeenCalledOnce();
  });

  it("not when the focus is in a text field or on a button (they use the space bar), nor with a dialog open", async () => {
    press(host.querySelector("input")!);
    press(host.querySelector("button")!);
    expect(calls.start).not.toHaveBeenCalled();
    const modal = document.createElement("div");
    modal.className = "modal-backdrop";
    document.body.append(modal);
    press();
    expect(calls.start).not.toHaveBeenCalled();
    modal.remove();
    press(document.body, { repeat: true }); press(document.body, { ctrlKey: true }); press(document.body, { key: "a", code: "KeyA" });
    expect(calls.start).not.toHaveBeenCalled();
    press();
    expect(calls.start).toHaveBeenCalledOnce();
  });

  it("stops listening when the hook unmounts", async () => {
    await act(async () => root.render(<div />));
    press();
    expect(calls.start).not.toHaveBeenCalled();
  });
});

describe("the camera the active profile expects and the configured focus staleness", () => {
  type Deps = Parameters<typeof useProfileCameraContext>[0];
  const setExpectedCamera = vi.fn(), setFocusStaleMs = vi.fn();
  beforeEach(() => { vi.mocked(bridge.fetchAutofocusConfig).mockResolvedValue({ valid: false, ring_ratio_stale_ms: 0 } as never); });
  function Context(props: Partial<Deps>) {
    useProfileCameraContext({
      ready: true, tab: "overview", caps: { autofocus: true }, profiles: { activeProfile: null }, afStatus: null, setExpectedCamera, setFocusStaleMs, ...props,
    } as never);
    return null;
  }
  const mount = (props: Partial<Deps> = {}) => act(async () => root.render(<Context {...props} />));
  const profile = (revision = "r1") => ({ activeProfile: { name: "include", path: "/profiles/include/config.json", revision } }) as never;

  it("the expected camera is the active profile's camera.window.camera_model", async () => {
    vi.mocked(configDocument.read).mockResolvedValue({
      ok: true, path: "/profiles/include/config.json", revision: "r1", error: "",
      document_json: JSON.stringify({ camera: { window: { camera_vendor: "SVS-Vistek GmbH", camera_model: "MC2066" } } }),
    });
    await mount({ profiles: profile() });
    expect(configDocument.read).toHaveBeenCalledWith("/profiles/include/config.json");
    expect(setExpectedCamera).toHaveBeenLastCalledWith("MC2066");
    // a new revision of the profile is read again
    vi.mocked(configDocument.read).mockResolvedValue({ ok: true, path: "", revision: "r2", error: "", document_json: JSON.stringify({ camera: {} }) });
    await mount({ profiles: profile("r2") });
    expect(setExpectedCamera).toHaveBeenLastCalledWith("");
  });

  it("no active profile, an unreadable document or a failed read expects nothing", async () => {
    await mount();
    expect(setExpectedCamera).toHaveBeenLastCalledWith("");
    expect(configDocument.read).not.toHaveBeenCalled();
    vi.mocked(configDocument.read).mockResolvedValue({ ok: false, path: "", revision: "", error: "gone", document_json: "" });
    await mount({ profiles: profile("r3") });
    expect(setExpectedCamera).toHaveBeenLastCalledWith("");
    vi.mocked(configDocument.read).mockRejectedValue(new Error("ipc"));
    await mount({ profiles: profile("r4") });
    expect(setExpectedCamera).toHaveBeenLastCalledWith("");
  });

  it("the focus gate's staleness is the autofocus config's ring_ratio_stale_ms", async () => {
    vi.mocked(bridge.fetchAutofocusConfig).mockResolvedValue({ valid: true, ring_ratio_stale_ms: 2500 } as never);
    await mount();
    expect(setFocusStaleMs).toHaveBeenLastCalledWith(2500);
  });

  it("an invalid or zero configuration, a failed read, or an instrument without autofocus keeps the last value", async () => {
    vi.mocked(bridge.fetchAutofocusConfig).mockResolvedValue({ valid: false, ring_ratio_stale_ms: 2500 } as never);
    await mount();
    vi.mocked(bridge.fetchAutofocusConfig).mockResolvedValue({ valid: true, ring_ratio_stale_ms: 0 } as never);
    await mount({ tab: "connect" });
    vi.mocked(bridge.fetchAutofocusConfig).mockRejectedValue(new Error("ipc"));
    await mount({ tab: "overview" });
    await mount({ caps: { autofocus: false } as never });
    await mount({ tab: "review" });
    expect(setFocusStaleMs).not.toHaveBeenCalled();
  });
});
