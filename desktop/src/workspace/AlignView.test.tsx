// @vitest-environment jsdom
// O2 Align (A3) over a fake frame sink: the image tools read the frames the classic preview
// delivers (probe, histogram, profile, fit / 1:1 / zoom, freeze), the quick control goes
// requested → pending → readback / rejected / timed out, and nothing is sent on its own.
import { act, StrictMode } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { invoke } from "@tauri-apps/api/core";
import type { CameraGeometry } from "../bridge";
import type { FramePacket } from "../framePacket";
import type { AlignFacts } from "./alignControls";
import { INSPECT_INTERVAL_MS } from "./alignImageModel";
import { AlignView, type AlignViewProps, type FrameSink } from "./AlignView";
import { WRITE_TIMEOUT_MS, type Rect, type WriteResult } from "./quickControlModel";

vi.mock("@tauri-apps/api/core", () => ({ invoke: vi.fn() }));

const GEOMETRY: CameraGeometry = {
  supported: true, overview: true, camera: "aravis", sensor_width: 2048, sensor_height: 512,
  roi: { x: 768, y: 208, width: 512, height: 96 }, width_increment: 16, height_increment: 4, offset_x_increment: 16, offset_y_increment: 4,
  min_width: 64, min_height: 4, session: { frame_rate_hz: 2000, exposure_us: 2 },
};
const FACTS: AlignFacts = {
  pz7035: false, instrumentModes: false, hostProcessing: true, geometry: GEOMETRY, instrumentMode: null, runFrozen: false, modeError: "",
  appliedRunWindow: null, placedRunWindow: null, processingRoi: null, runFps: null, experimentActive: false, running: true,
};

let host: HTMLDivElement, root: Root, sink: FrameSink | null;
const subscribeFrames = vi.fn((s: FrameSink | null) => { sink = s; });
const onSwitchMode = vi.fn(async () => {});
const onClassic = vi.fn();
let writeRoi: ReturnType<typeof vi.fn<(kind: string, r: Rect) => Promise<WriteResult<Rect>>>>;

const props = (over: Partial<AlignViewProps> = {}): AlignViewProps =>
  ({ facts: FACTS, subscribeFrames, onSwitchMode, writeRoi, lockedReason: "", onClassic, ...over });
// StrictMode, as main.tsx renders the app: effects mount, unmount and mount again.
const render = (over: Partial<AlignViewProps> = {}) => act(async () => root.render(<StrictMode><AlignView {...props(over)} /></StrictMode>));
const advance = (ms: number) => act(async () => { await vi.advanceTimersByTimeAsync(ms); });
const $ = (id: string) => host.querySelector(`[data-testid=${id}]`) as HTMLElement;
const button = (text: string) => [...host.querySelectorAll("button")].find((b) => b.textContent === text)!;
const press = (text: string) => act(async () => button(text).click());
const subtitle = () => host.querySelector(".ws-align__image .mib-card__subtitle")!.textContent;
const field = (k: string) => host.querySelector<HTMLInputElement>(`input[aria-label$=" ${k}"]`)!;
async function type(k: string, value: string) {
  const el = field(k);
  await act(async () => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(el, value);
    el.dispatchEvent(new Event("input", { bubbles: true }));
  });
}
const chip = (id: string) => $(`align-state-${id}`);

/** A delivered live packet: pixel (x, y) = 4x (Mono8), 64 × 32, with row padding. */
function packet(index: string, width = 64, height = 32): FramePacket {
  const stride = width + 4;
  const data = new Uint8Array(stride * height);
  for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) data[y * stride + x] = (4 * x) & 255;
  return { valid: true, frame_index: index, timestamp_ns: "0", timestamp_validity: "unavailable", source_id: null, session_id: null,
    store_generation: null, config_revision: null, pull_kind: 1, width, height, pixel_format: 0x01080001, stride_bytes: stride, byte_len: data.length, data };
}
const deliver = (p: FramePacket) => act(async () => { sink!(p); });
const pointer = (type: string, x: number, y: number) =>
  act(async () => { $("align-canvas").dispatchEvent(new MouseEvent(type, { bubbles: true, clientX: x, clientY: y })); });

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  vi.useFakeTimers();
  vi.mocked(invoke).mockReset();
  vi.spyOn(HTMLCanvasElement.prototype, "getContext").mockReturnValue(null); // jsdom draws nothing
  subscribeFrames.mockClear(); onSwitchMode.mockClear(); onClassic.mockClear();
  writeRoi = vi.fn();
  sink = null;
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => {
  await act(async () => root.unmount());
  host.remove();
  vi.useRealTimers();
  vi.restoreAllMocks();
});

describe("image tools on the classic preview's frames", () => {
  it("registers one sink, and unregisters it on unmount", async () => {
    await render();
    // StrictMode subscribes, unsubscribes and subscribes again: one live sink remains.
    expect(subscribeFrames.mock.calls.map(([s]) => typeof s)).toEqual(["function", "object", "function"]);
    expect(sink).toBeTypeOf("function");
    await act(async () => root.unmount());
    expect(subscribeFrames).toHaveBeenLastCalledWith(null);
    root = createRoot(host);
  });

  it("shows the frame with its probe, histogram and line profile, and sends no command", async () => {
    await render();
    expect($("align-no-frame").textContent).toBe("Waiting for the first frame…");
    await deliver(packet("5"));
    await advance(INSPECT_INTERVAL_MS);
    // Fit: 64 × 32 in the 640 × 300 viewport is 9.375×, centred horizontally (offset 20).
    expect(subtitle()).toBe("Frame 5 · 64 × 32 · Mono8 · fit 938 %");
    expect($("align-histogram").textContent).toContain("min 0 · max 252 · mean 126.0 · saturated 0.0 % · 2,048 px (every pixel)");
    expect($("align-profile").textContent).toContain("Row y 16 · 64 points · min 0 · max 252");
    await pointer("pointermove", 20 + 9.375 * 10 + 1, 9.375 * 5 + 1);
    expect($("align-probe").textContent).toBe("Pixel probe · x 10 · y 5 · value 40");
    // A click pins the point; the profile runs through its row, or its column.
    await pointer("pointerdown", 20 + 9.375 * 10 + 1, 9.375 * 5 + 1);
    await pointer("pointerup", 20 + 9.375 * 10 + 1, 9.375 * 5 + 1);
    await advance(INSPECT_INTERVAL_MS);
    expect($("align-profile").textContent).toContain("Row y 5 · 64 points");
    expect($("align-probe").textContent).toContain("pinned x 10 · y 5 · value 40");
    await press("Profile: column");
    await advance(INSPECT_INTERVAL_MS);
    expect($("align-profile").textContent).toContain("Column x 10 · 32 points · min 40 · max 40");
    expect(invoke).not.toHaveBeenCalled();
  });

  it("fit, 1:1 and zoom change the view, never the frame", async () => {
    await render();
    await deliver(packet("5"));
    await advance(INSPECT_INTERVAL_MS);
    await act(async () => host.querySelector<HTMLButtonElement>("button[aria-label='Zoom in']")!.click());
    expect(subtitle()).toBe("Frame 5 · 64 × 32 · Mono8 · zoom 1172 %");
    await press("1:1");
    expect(subtitle()).toBe("Frame 5 · 64 × 32 · Mono8 · 1:1 100 %");
    await press("Fit");
    expect(subtitle()).toBe("Frame 5 · 64 × 32 · Mono8 · fit 938 %");
    // Dragging pans (and leaves fit).
    await pointer("pointerdown", 100, 100);
    await pointer("pointermove", 60, 100);
    await pointer("pointerup", 60, 100);
    expect(subtitle()).toContain("zoom 938 %");
  });

  it("freeze keeps the shown frame for inspection while frames keep arriving", async () => {
    await render();
    await deliver(packet("5"));
    await advance(INSPECT_INTERVAL_MS);
    await press("Freeze display");
    await deliver(packet("6"));
    await advance(INSPECT_INTERVAL_MS * 4);
    expect(subtitle()).toBe("Frame 5 · 64 × 32 · Mono8 · fit 938 % · display frozen");
    await press("Resume display");
    await deliver(packet("7"));
    await advance(INSPECT_INTERVAL_MS);
    expect(subtitle()).toBe("Frame 7 · 64 × 32 · Mono8 · fit 938 %");
  });

  it("a frozen image keeps its size when the camera's frames change size (e.g. Show the full sensor)", async () => {
    await render();
    await deliver(packet("5"));
    await advance(INSPECT_INTERVAL_MS);
    await press("Freeze display");
    const accepted = await act(async () => sink!(packet("6", 256, 8)));
    expect(accepted).toBe(false);
    await advance(INSPECT_INTERVAL_MS * 4);
    // The view still fits 64 × 32 (9.375×, offset 20) and the probe still maps onto its pixels.
    expect(subtitle()).toBe("Frame 5 · 64 × 32 · Mono8 · fit 938 % · display frozen");
    await pointer("pointermove", 20 + 9.375 * 10 + 1, 9.375 * 5 + 1);
    expect($("align-probe").textContent).toBe("Pixel probe · x 10 · y 5 · value 40");
    await press("Fit");
    expect(subtitle()).toBe("Frame 5 · 64 × 32 · Mono8 · fit 938 % · display frozen");
    await press("Resume display");
    expect(await act(async () => sink!(packet("7", 256, 8)))).toBe(true);
    await advance(INSPECT_INTERVAL_MS);
    expect(subtitle()).toBe("Frame 7 · 256 × 8 · Mono8 · fit 250 %");
  });

  it("a burst of frames is inspected at most once per interval", async () => {
    await render();
    for (let i = 0; i < 50; i++) await deliver(packet(String(i)));
    await advance(INSPECT_INTERVAL_MS);
    expect(subtitle()).toContain("Frame 49 ·"); // the newest; the others were skipped, not queued
  });

  it("says why there is no frame", async () => {
    await render({ facts: { ...FACTS, running: false } });
    expect($("align-no-frame").textContent).toBe("No live frame: the camera is not running. Start it from the classic view.");
    await render({ facts: { ...FACTS, running: false, pz7035: true, instrumentModes: true, instrumentMode: "run" } });
    expect($("align-no-frame").textContent).toBe("No live camera image outside Align. Switch to Align above.");
  });
});

describe("quick controls: requested → readback", () => {
  it("lists the features with their access: read-only, unavailable, writable", async () => {
    await render();
    expect(chip("exposure").textContent).toBe("Read-only · camera script");
    expect(chip("exposure").getAttribute("title")).toBe("Read-only: set in the camera script or configuration (classic view)");
    expect(chip("exposure").getAttribute("data-tone")).toBe("neutral");
    expect(chip("gain").getAttribute("data-tone")).toBe("unsupported");
    expect(chip("gain").textContent).toBe("Unavailable · not bridged");
    expect(chip("gain").getAttribute("title")).toMatch(/^Unavailable: not exposed through the bridge/);
    expect(chip("roi").textContent).toBe("Current");
    expect(field("X").value).toBe("768");
    expect($("align-quick-controls").textContent).toContain("2 µs");
  });

  it("writes only on Write, shows pending, then the readback read after the write", async () => {
    let answer!: (r: WriteResult<Rect>) => void;
    writeRoi.mockImplementation(() => new Promise((r) => { answer = r; }));
    await render();
    await type("X", "790");
    expect(writeRoi).not.toHaveBeenCalled();
    expect(chip("roi").textContent).toBe("Edited · not written");
    await press("Write ROI");
    expect(writeRoi).toHaveBeenCalledWith("camera", { x: 784, y: 208, width: 512, height: 96 }); // snapped to the step of 16
    expect(chip("roi").textContent).toBe("Pending · waiting for the readback");
    expect(field("X").disabled).toBe(true);
    await act(async () => answer({ ok: true, readback: { x: 784, y: 208, width: 512, height: 96 } }));
    expect(chip("roi").textContent).toBe("Readback matches the request");
    expect(chip("roi").getAttribute("data-tone")).toBe("confirmed");
    expect($("align-snap-note").textContent).toBe("Written after snapping: X snapped to 784 px (step 16).");
    expect(invoke).not.toHaveBeenCalled(); // the write went through the caller's (classic) path
  });

  it("a rejected write shows its reason and restores the readback", async () => {
    writeRoi.mockResolvedValue({ ok: false, reason: "Camera ROI was not saved" });
    await render();
    await type("X", "784");
    await press("Write ROI");
    expect(chip("roi").textContent).toBe("Rejected · Camera ROI was not saved");
    expect(chip("roi").getAttribute("data-tone")).toBe("blocked");
    expect(field("X").value).toBe("768");
  });

  it("a write without a reply times out, restores the readback and ignores the late reply", async () => {
    let answer!: (r: WriteResult<Rect>) => void;
    writeRoi.mockImplementation(() => new Promise((r) => { answer = r; }));
    await render();
    await type("X", "784");
    await press("Write ROI");
    await advance(WRITE_TIMEOUT_MS);
    expect(chip("roi").textContent).toBe("Timed out · no reply within 5 s · showing the readback");
    expect(field("X").value).toBe("768");
    await act(async () => answer({ ok: true, readback: { x: 784, y: 208, width: 512, height: 96 } }));
    expect(chip("roi").textContent).toMatch(/^Timed out/);
  });

  it("refuses an out-of-range value before sending anything", async () => {
    await render();
    await type("X", "4000");
    await press("Write ROI");
    expect(writeRoi).not.toHaveBeenCalled();
    expect(chip("roi").textContent).toBe("Not written · X: out of range · 0–1536 px");
    await press("Revert to readback");
    expect(field("X").value).toBe("768");
  });

  it("is locked while a run is active", async () => {
    await render({ lockedReason: "an experiment is active" });
    expect(field("X").disabled).toBe(true);
    expect(button("Write ROI").disabled).toBe(true);
    expect($("align-locked").textContent).toBe("Writes are locked: an experiment is active.");
  });

  it("PZ7035: W and H of the Run window are fixed; a placement says it applies at Run", async () => {
    writeRoi.mockResolvedValue({ ok: true, readback: null });
    await render({ facts: { ...FACTS, pz7035: true, instrumentModes: true, hostProcessing: false, instrumentMode: "align",
      geometry: { ...GEOMETRY, sensor_width: 816, sensor_height: 624 } } });
    expect(field("W").disabled).toBe(true);
    expect(field("H").disabled).toBe(true);
    expect(chip("roi").textContent).toBe("Unknown · not reported");
    await type("X", "41");
    await press("Write Run window");
    expect(writeRoi).toHaveBeenCalledWith("run", { x: 40, y: 264, width: 512, height: 96 });
    expect(chip("roi").textContent).toBe("Written · applied when Experiment opens (the switch to Run)");
    expect(chip("roi").getAttribute("data-tone")).toBe("info");
  });
});

describe("the camera mode switch is a button", () => {
  it("offers the classic tab's switch and acts only on the press", async () => {
    await render({ facts: { ...FACTS, geometry: { ...GEOMETRY, overview: false } } });
    expect($("align-mode").textContent).toContain("Camera · Experiment window");
    await advance(5000);
    expect(onSwitchMode).not.toHaveBeenCalled();
    await press("Show the full sensor");
    expect(onSwitchMode).toHaveBeenCalledTimes(1);
  });

  it("is disabled, with the reason, while a run is active", async () => {
    await render({ facts: { ...FACTS, geometry: { ...GEOMETRY, overview: false }, experimentActive: true } });
    expect(button("Show the full sensor").disabled).toBe(true);
    expect($("align-mode").textContent).toContain("Show the full sensor is unavailable: a run is active.");
  });

  it("shows no switch when the image is already the full sensor", async () => {
    await render();
    expect($("align-mode")).toBeNull();
    expect($("align-mode-chip").textContent).toBe("Full sensor 2048 × 512");
  });

  it("All features leaves for the classic view", async () => {
    await render();
    await press("All features · classic view");
    expect(onClassic).toHaveBeenCalledTimes(1);
  });
});
