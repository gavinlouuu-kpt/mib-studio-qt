// @vitest-environment jsdom
// Wafer Grid on the Overview (CONNECT-054): the hook and the toolbar button against a mocked bridge.
// Decoding is switched on and off with the Overview's visibility, the pose is polled at about 4 Hz only while
// enabled and shown, an unchanged pose is not copied, a refused toggle stays visible, a viewer sends nothing.
import { act, type ReactNode } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { DotGridPose } from "../../bridge";
import { StudioContext } from "../../studio/StudioContext";
import { useWaferGrid, type WaferGridModel } from "./useWaferGrid";
import { WaferGridNotice, WaferGridToggle } from "./WaferGridControls";
import { WAFER_GRID_POLL_MS } from "./waferGrid";

const head = { available: true, paused: false, designs: 1, unchanged: false };
const off = (): DotGridPose => ({ ...head, enabled: false, has_pose: false, sequence: 0 });
const pose = (sequence: number, over: Partial<DotGridPose> = {}): DotGridPose => ({
  ...head, enabled: true, has_pose: true, sequence, valid: true, image_width: 800, image_height: 600, dots: 40, votes: 4, decode_ms: 3,
  reason: "", centre_x_um: 40000, centre_y_um: 52000, theta_deg: 5, um_per_px: 0.293, mirrored: true, chip: "R3C0", design_name: "Wafer_soRT",
  dots_px: [[10, 20]], dots_px_total: 1, ...over,
});

const bridgeMock = vi.hoisted(() => ({
  setDotGridEnabled: vi.fn(), setDotGridPaused: vi.fn(), fetchDotGridPose: vi.fn(), fetchFrame: vi.fn(),
}));
vi.mock("../../bridge", () => ({ bridge: bridgeMock }));

type Props = Parameters<typeof useWaferGrid>[0];
let host: HTMLDivElement, root: Root, model: WaferGridModel;
let props: Props;
let server: { enabled: boolean; sequence: number };
let visibility: DocumentVisibilityState;

function Harness({ children }: { children?: ReactNode }) {
  model = useWaferGrid(props);
  const studio = {
    waferGrid: model.waferGrid, waferGridSupported: model.waferGridSupported, waferGridError: model.waferGridError,
    onToggleWaferGrid: model.onToggleWaferGrid, ready: props.ready, expActive: props.expActive,
  };
  return <StudioContext.Provider value={studio as never}>{children}</StudioContext.Provider>;
}
const mount = async (over: Partial<Props> = {}) => {
  props = { ...props, ...over };
  await act(async () => root.render(<Harness><WaferGridToggle /><WaferGridNotice /></Harness>));
};
const tick = async (ms: number) => { await act(async () => { await vi.advanceTimersByTimeAsync(ms); }); };
const button = () => host.querySelector<HTMLButtonElement>('[data-testid="wafer-grid-toggle"]');
const pausedCalls = () => bridgeMock.setDotGridPaused.mock.calls.map((c) => c[0] as boolean);
const overlay = () => props.waferGridOverlayRef.current;

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  vi.useFakeTimers();
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
  server = { enabled: false, sequence: 0 };
  visibility = "visible";
  Object.defineProperty(document, "visibilityState", { configurable: true, get: () => visibility });
  Object.values(bridgeMock).forEach((m) => m.mockReset());
  bridgeMock.setDotGridPaused.mockResolvedValue({ ok: true, message: "" });
  bridgeMock.setDotGridEnabled.mockImplementation(async (enabled: boolean) => { server.enabled = enabled; return { ok: true, message: "" }; });
  bridgeMock.fetchDotGridPose.mockImplementation(async (since = 0) => {
    if (!server.enabled) return off();
    if (since === server.sequence) return { ...head, enabled: true, has_pose: true, sequence: server.sequence, unchanged: true };
    return pose(server.sequence);
  });
  props = {
    ready: true, tab: "overview", caps: { frame_buffer: true } as never, append: vi.fn(), expActive: false, viewerOnly: false,
    waferGridOverlayRef: { current: null }, framePulls: { current: { request: vi.fn() } } as never,
  };
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.useRealTimers(); });

describe("decoding follows the Overview", () => {
  it("resumes the service when the Overview is shown and pauses it on a stage change", async () => {
    await mount({ tab: "connect" });
    expect(pausedCalls()).toEqual([true]);
    await mount({ tab: "overview" });
    expect(pausedCalls()).toEqual([true, false]);
    await mount({ tab: "experiment" });
    expect(pausedCalls()).toEqual([true, false, true]);
    expect(bridgeMock.fetchDotGridPose).toHaveBeenCalledTimes(1); // only the read on arrival, while the Overview was shown
  });

  it("pauses when the window is hidden or minimised and resumes when it is shown again", async () => {
    await mount();
    expect(pausedCalls()).toEqual([false]);
    visibility = "hidden";
    await act(async () => { document.dispatchEvent(new Event("visibilitychange")); });
    expect(pausedCalls()).toEqual([false, true]);
    visibility = "visible";
    await act(async () => { document.dispatchEvent(new Event("visibilitychange")); });
    expect(pausedCalls()).toEqual([false, true, false]);
  });

  it("pauses during an experiment, when the frame source is not the host's, and sends nothing for a viewer", async () => {
    await mount({ expActive: true });
    expect(pausedCalls()).toEqual([true]);
    expect(overlay()).toBeNull();
    bridgeMock.setDotGridPaused.mockClear();
    await mount({ expActive: false, caps: { frame_buffer: false } as never });
    expect(bridgeMock.setDotGridPaused).not.toHaveBeenCalled();
    expect(button()).toBeNull(); // PZ7035 Align: no button
    await mount({ caps: { frame_buffer: true } as never, viewerOnly: true });
    expect(bridgeMock.setDotGridPaused).not.toHaveBeenCalled();
  });

  it("pauses when the view is left for good (the Qt destructor)", async () => {
    await mount();
    bridgeMock.setDotGridPaused.mockClear();
    await act(async () => root.unmount());
    expect(pausedCalls()).toEqual([true]);
    host.remove(); host = document.createElement("div"); document.body.append(host); root = createRoot(host); // the afterEach unmounts a root
  });
});

describe("the toggle and the pose poll", () => {
  it("polls about four times a second only while enabled, copies a pose only when its sequence moved, and draws it", async () => {
    await mount();
    expect(button()!.textContent).toBe("Wafer Grid: Off");
    expect(button()!.getAttribute("aria-pressed")).toBe("false");
    await tick(2 * WAFER_GRID_POLL_MS);
    expect(bridgeMock.fetchDotGridPose).toHaveBeenCalledTimes(1); // switched off: no polling
    expect(overlay()).not.toBeNull();

    server.sequence = 3;
    await act(async () => { button()!.click(); });
    expect(bridgeMock.setDotGridEnabled).toHaveBeenCalledWith(true);
    expect(button()!.textContent).toBe("Wafer Grid: On");
    expect(button()!.getAttribute("aria-pressed")).toBe("true");
    expect(props.append).toHaveBeenCalledWith("Wafer Grid on");
    expect(props.framePulls.current.request).toHaveBeenCalled(); // repaint for a stopped camera

    bridgeMock.fetchDotGridPose.mockClear();
    await tick(4 * WAFER_GRID_POLL_MS);
    expect(bridgeMock.fetchDotGridPose).toHaveBeenCalledTimes(4);
    // Every poll sends the sequence of the pose already copied, so an unchanged one comes back without dots.
    expect(bridgeMock.fetchDotGridPose.mock.calls.map((c) => c[0])).toEqual([3, 3, 3, 3]);

    // The overlay draws the copied pose on the live canvas.
    const drawn: string[] = [];
    const ctx = { canvas: { width: 800, height: 600, clientWidth: 800 }, save: () => undefined, restore: () => undefined, beginPath: () => undefined,
      arc: () => drawn.push("arc"), stroke: () => undefined, moveTo: () => undefined, lineTo: () => undefined, measureText: () => ({ width: 50 }),
      fillRect: () => undefined, fillText: (t: string) => drawn.push(t) } as unknown as CanvasRenderingContext2D;
    overlay()!(ctx);
    expect(drawn).toContain("arc");
    expect(drawn.some((t) => t.startsWith("Wafer X 40000.0"))).toBe(true);

    // A newer pose moves the sequence; the next poll asks from there.
    server.sequence = 4;
    bridgeMock.fetchDotGridPose.mockClear();
    await tick(2 * WAFER_GRID_POLL_MS);
    expect(bridgeMock.fetchDotGridPose.mock.calls.map((c) => c[0])).toEqual([3, 4]);
  });

  it("switching off clears the overlay and stops the polling", async () => {
    await mount();
    await act(async () => { button()!.click(); });
    await act(async () => { button()!.click(); });
    expect(bridgeMock.setDotGridEnabled).toHaveBeenLastCalledWith(false);
    expect(button()!.textContent).toBe("Wafer Grid: Off");
    const drawn: string[] = [];
    overlay()!({ canvas: { width: 800, height: 600, clientWidth: 800 }, fillText: (t: string) => drawn.push(t) } as unknown as CanvasRenderingContext2D);
    expect(drawn).toEqual([]);
    bridgeMock.fetchDotGridPose.mockClear();
    await tick(4 * WAFER_GRID_POLL_MS);
    expect(bridgeMock.fetchDotGridPose).not.toHaveBeenCalled();
  });

  it("learns that the service is already on (another client, an earlier page load) from the read on arrival", async () => {
    server = { enabled: true, sequence: 5 };
    await mount();
    expect(button()!.textContent).toBe("Wafer Grid: On");
    bridgeMock.fetchDotGridPose.mockClear();
    await tick(2 * WAFER_GRID_POLL_MS);
    expect(bridgeMock.fetchDotGridPose).toHaveBeenCalledTimes(2);
  });

  it("shows a refused toggle as text until a toggle works", async () => {
    bridgeMock.setDotGridEnabled.mockResolvedValueOnce({ ok: false, message: "bundled dot-grid registry rejected: bad seed" });
    await mount();
    await act(async () => { button()!.click(); });
    const notice = host.querySelector('[data-testid="wafer-grid-error"]')!;
    expect(notice.textContent).toBe("Dot-grid localization is not available: bundled dot-grid registry rejected: bad seed");
    expect(notice.getAttribute("role")).toBe("alert");
    expect(props.append).toHaveBeenCalledWith("Dot-grid localization is not available: bundled dot-grid registry rejected: bad seed");
    expect(button()!.textContent).toBe("Wafer Grid: Off");
    await act(async () => { button()!.click(); });
    expect(host.querySelector('[data-testid="wafer-grid-error"]')).toBeNull();
    expect(button()!.textContent).toBe("Wafer Grid: On");
  });

  it("shows a bridge failure the same way", async () => {
    bridgeMock.setDotGridEnabled.mockRejectedValueOnce(new Error("VIEWER_ONLY: another client controls the instrument"));
    await mount();
    await act(async () => { button()!.click(); });
    expect(host.querySelector('[data-testid="wafer-grid-error"]')!.textContent).toBe(
      "Dot-grid localization is not available: VIEWER_ONLY: another client controls the instrument");
  });

  it("disables the button during an experiment and when the service is not running in this build", async () => {
    await mount({ expActive: true });
    expect(button()!.disabled).toBe(true);
    expect(button()!.title).toBe("Not during an experiment");
    bridgeMock.fetchDotGridPose.mockResolvedValue({ ...off(), available: false, designs: 0 });
    await mount({ expActive: false });
    expect(button()!.disabled).toBe(true);
    expect(button()!.title).toContain("switched off in this build");
  });
});
