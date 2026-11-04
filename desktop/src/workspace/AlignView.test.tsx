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
import { AlignView, PROCESSING_ROI_WRITE, type AlignViewProps, type FrameSink } from "./AlignView";
import { WRITE_TIMEOUT_MS, type Rect, type WriteResult } from "./quickControlModel";
import type { CameraFeatureApi } from "./useCameraFeatures";
import type { CameraFeatureReply, CameraFeatureTreeReply, CameraFeatureWriteRequest } from "../bridge";
import { FEATURES, SESSION, treeOutcome, treeReply, writeReply } from "./cameraFeatureFixtures";

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
  it("without the feature tree connected, the SFNC rows say so and show the camera's geometry report", async () => {
    await render();
    expect(chip("exposure").textContent).toBe("Unknown · the camera's feature tree is not connected here");
    expect(chip("exposure").getAttribute("data-tone")).toBe("unknown");
    expect(chip("gain").textContent).toBe("Unknown · the camera's feature tree is not connected here");
    expect(chip("roi").textContent).toBe("Current");
    expect(field("X").value).toBe("768");
    // The geometry's exposure report, labelled as not from the node map.
    expect($("align-quick-controls").textContent).toContain("2 µscamera report, not from the node map");
    expect($("align-quick-controls").textContent).not.toContain("classic view, as are all features");
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

  it("Classic view leaves for the classic view; nothing claims the features live there", async () => {
    await render();
    expect(host.textContent).not.toContain("All features");
    await press("Classic view");
    expect(onClassic).toHaveBeenCalledTimes(1);
  });
});

describe("Write ROI says what else it does (#763 F16)", () => {
  it("the processing ROI's Write says it also starts realtime processing, as the classic Apply ROI; writing still waits for the press", async () => {
    await render({ facts: { ...FACTS, geometry: null, processingRoi: { x: 0, y: 0, width: 0, height: 0 } } });
    expect($("align-roi-write-note").textContent).toBe(PROCESSING_ROI_WRITE);
    expect(PROCESSING_ROI_WRITE).toContain("as the classic Apply ROI does: it also starts realtime processing");
    expect(button("Write ROI").title).toBe(PROCESSING_ROI_WRITE);
    expect(writeRoi).not.toHaveBeenCalled();
  });

  it("the camera ROI and the PZ7035 Run window write only the window: no such note", async () => {
    await render();
    expect($("align-roi-write-note")).toBeNull();
    await render({ facts: { ...FACTS, pz7035: true, instrumentModes: true, geometry: null, instrumentMode: "align" } });
    expect($("align-roi-write-note")).toBeNull();
  });
});

describe("SFNC quick controls over the camera's node map (#806 E-3)", () => {
  /** A backend double. Trees: `trees[0]` until the first write, then each later entry once in
   *  turn (the last repeats). StrictMode reads the tree twice on mount; neither moves the queue. */
  function fakeFeatures(trees: CameraFeatureTreeReply[], write: (r: CameraFeatureWriteRequest) => CameraFeatureReply | Promise<CameraFeatureReply>) {
    let queue = [trees[0]];
    const api = {
      tree: vi.fn(async () => (queue.length > 1 ? queue.shift()! : queue[0])),
      write: vi.fn(async (r: CameraFeatureWriteRequest) => {
        if (trees.length > 1) queue = trees.slice(1);
        return write(r);
      }),
    } satisfies CameraFeatureApi;
    return api;
  }
  const settle = () => act(async () => { await vi.advanceTimersByTimeAsync(0); });
  const draft = (id: string) => host.querySelector<HTMLInputElement | HTMLSelectElement>(`[data-testid=align-draft-${id}] input, [data-testid=align-draft-${id}] select`)!;
  async function setDraft(id: string, value: string) {
    const el = draft(id);
    const proto = el instanceof HTMLSelectElement ? HTMLSelectElement.prototype : HTMLInputElement.prototype;
    await act(async () => {
      Object.getOwnPropertyDescriptor(proto, "value")!.set!.call(el, value);
      el.dispatchEvent(new Event(el instanceof HTMLSelectElement ? "change" : "input", { bubbles: true }));
    });
  }
  const writeButton = (label: string) => host.querySelector<HTMLButtonElement>(`button[aria-label="Write ${label}"]`)!;

  it("reads the tree on open (a read), resolves each SFNC name and writes nothing", async () => {
    const api = fakeFeatures([treeReply({ features: [FEATURES.ExposureTime, FEATURES.TriggerMode, FEATURES.AcquisitionFrameRate] })],
      () => { throw new Error("no write expected"); });
    await render({ features: api });
    await settle();
    expect(api.tree).toHaveBeenCalledWith({ modules: ["remote"] });
    expect(api.write).not.toHaveBeenCalled();
    expect(chip("exposure").textContent).toBe("Writable");
    expect(draft("exposure").value).toBe("1000");
    expect(chip("gain").textContent).toBe("Not available on this camera");
    expect(draft("gain")).toBeNull();
    expect(chip("trigger").textContent).toBe("Writable");
    expect(draft("trigger").value).toBe("Off");
    expect($("align-roi-nodemap").textContent).toBe("Node map: sensor ROI not available on this camera");
  });

  it("a read-only node shows the device's access reason and offers no draft", async () => {
    const ro = { ...FEATURES.ExposureTime, descriptor: { ...FEATURES.ExposureTime.descriptor!, writable: false, access_reason: "Not writable while ExposureAuto is Continuous." } };
    await render({ features: fakeFeatures([treeReply({ features: [ro] })], () => { throw new Error(); }) });
    await settle();
    expect(chip("exposure").textContent).toBe("Read-only · Not writable while ExposureAuto is Continuous.");
    expect(draft("exposure")).toBeNull();
    expect($("align-quick-controls").textContent).toContain("1000 us");
  });

  it("commits a draft with the tree's token on Write, shows requested and readback as returned, and re-reads the tree", async () => {
    const after = treeReply({ session: { ...SESSION, capability_revision: "2" },
      features: [{ ...FEATURES.ExposureTime, descriptor: { ...FEATURES.ExposureTime.descriptor!, value: { type: "float", value: 248 } } }] });
    const api = fakeFeatures([treeReply(), after], (r) =>
      writeReply("readback", r.feature, r.value, { ...FEATURES.ExposureTime.descriptor!, value: { type: "float", value: 248 } }, "",
        { ...SESSION, capability_revision: "2" }));
    await render({ features: api });
    await settle();
    await setDraft("exposure", "250");
    expect(api.write).not.toHaveBeenCalled(); // nothing goes out until Write
    const reads = api.tree.mock.calls.length;
    await act(async () => writeButton("Exposure").click());
    await settle();
    expect(api.write).toHaveBeenCalledWith({ session: SESSION, module: "remote", feature: "ExposureTime", value: { type: "float", value: 250 } });
    expect($("align-write-exposure").textContent).toBe("Written · readback · requested 250 us · 248 us");
    expect($("align-write-exposure").getAttribute("data-tone")).toBe("confirmed");
    expect(api.tree).toHaveBeenCalledTimes(reads + 1); // refreshed after the write
    expect(draft("exposure").value).toBe("248"); // the draft follows the new tree
  });

  it("an out-of-range value is sent and the backend's refusal is shown with its reason", async () => {
    const api = fakeFeatures([treeReply()], (r) =>
      writeReply("refused", r.feature, r.value, FEATURES.Gain.descriptor!, "Value is outside the current device bounds."));
    await render({ features: api });
    await settle();
    await setDraft("gain", "99");
    await act(async () => writeButton("Gain").click());
    await settle();
    expect(api.write).toHaveBeenCalledTimes(1);
    expect($("align-write-gain").textContent).toBe("Refused · requested 99 dB · 0 dB · Value is outside the current device bounds.");
    expect($("align-write-gain").getAttribute("data-tone")).toBe("blocked");
  });

  it("a stale write reply re-reads the tree; a stale tree is read again at once", async () => {
    const api = fakeFeatures([treeReply(), treeOutcome("stale", "Source, session or capability revision changed during enumeration; enumerate again."),
      treeReply({ session: { ...SESSION, generation: "2" } })], (r) =>
      writeReply("stale", r.feature, r.value, null, "Backend instance, source, session or capability revision changed; refresh before retrying.",
        { ...SESSION, generation: "2" }));
    await render({ features: api });
    await settle();
    await setDraft("trigger", "On");
    const reads = api.tree.mock.calls.length;
    await act(async () => writeButton("Trigger mode").click());
    await settle();
    expect(api.write).toHaveBeenCalledWith(expect.objectContaining({ feature: "TriggerMode", value: { type: "string", value: "On" } }));
    expect($("align-write-trigger").textContent).toContain("Stale · requested On · no readback");
    expect(api.tree).toHaveBeenCalledTimes(reads + 2); // after the write, and again after the stale tree
    expect(chip("trigger").textContent).toBe("Writable");
  });

  it("an unknown write outcome is shown as unknown, never as written", async () => {
    const api = fakeFeatures([treeReply()], (r) => writeReply("unknown_write_outcome", r.feature, r.value, null, "write transport outcome unknown"));
    await render({ features: api });
    await settle();
    await setDraft("frameRate", "60");
    await act(async () => writeButton("Frame rate").click());
    await settle();
    expect($("align-write-frameRate").textContent).toBe("Unknown outcome · it may have reached the camera · requested 60 Hz · no readback · write transport outcome unknown");
    expect($("align-write-frameRate").getAttribute("data-tone")).toBe("unknown");
  });

  it("a write the server refused before acting (VIEWER_ONLY) is shown as not sent, never as a possible camera write", async () => {
    const api = fakeFeatures([treeReply()], () => Promise.reject(new Error("VIEWER_ONLY: another client controls the instrument")));
    await render({ features: api });
    await settle();
    await setDraft("exposure", "250");
    await act(async () => writeButton("Exposure").click());
    await settle();
    const w = $("align-write-exposure");
    expect(w.textContent).toBe("Not sent · another client controls this instrument · requested 250 us · no readback · another client controls the instrument");
    expect(w.getAttribute("data-tone")).toBe("blocked");
    expect(w.textContent).not.toContain("may have reached the camera");
  });

  it("a write with no reply (transport failure) is a transport state, not an unknown camera outcome", async () => {
    const api = fakeFeatures([treeReply()], () => Promise.reject(new Error("WebSocket closed")));
    await render({ features: api });
    await settle();
    await setDraft("gain", "3");
    await act(async () => writeButton("Gain").click());
    await settle();
    const w = $("align-write-gain");
    expect(w.textContent).toBe("No reply from the backend · requested 3 dB · no readback · WebSocket closed");
    expect(w.textContent).not.toContain("Unknown outcome");
  });

  it("a tree that stays stale is re-read a bounded number of times, then the chip says to Refresh", async () => {
    const stale = treeOutcome("stale", "Source, session or capability revision changed during enumeration; enumerate again.");
    const api = fakeFeatures([stale], () => { throw new Error(); });
    await render({ features: api });
    await settle();
    expect(chip("exposure").textContent).toBe("Stale · reading the tree again"); // a re-read is due
    await advance(5000);
    const calls = api.tree.mock.calls.length;
    expect(chip("exposure").textContent).toBe("Stale · press Refresh to read the tree again");
    await advance(60_000);
    expect(api.tree.mock.calls.length).toBe(calls); // the retries stopped
    await press("Feature tree");
    await act(async () => button("Refresh").click());
    await settle();
    expect(api.tree.mock.calls.length).toBeGreaterThan(calls); // Refresh reads again
  });

  it("the backend's refusal (sort pulses pending) is shown in Align as the backend wrote it", async () => {
    const reason = "Camera feature access is refused while sort pulses are pending: a node-map call would delay them.";
    await render({ features: fakeFeatures([treeOutcome("refused", reason)], () => { throw new Error(); }) });
    await settle();
    expect(chip("exposure").textContent).toBe(`Refused · ${reason}`);
    expect(chip("exposure").getAttribute("title")).toBe(`The backend refused: ${reason}`);
    expect(draft("exposure")).toBeNull(); // nothing to write against a refused tree
    expect($("align-features-refused").textContent).toBe(`The backend refused camera feature access: ${reason}`);
    await press("Feature tree");
    expect($("align-feature-tree-status").textContent).toBe(`refused · ${reason}`);
  });

  it("a write refused by the backend for a run in progress keeps the backend's reason", async () => {
    const reason = "Camera feature access is refused while the experiment is starting.";
    const api = fakeFeatures([treeReply()], (r) => writeReply("refused", r.feature, r.value, null, reason));
    await render({ features: api });
    await settle();
    await setDraft("exposure", "250");
    await act(async () => writeButton("Exposure").click());
    await settle();
    expect($("align-write-exposure").textContent).toBe(`Refused · requested 250 us · no readback · ${reason}`);
  });

  it("is locked while a run is active", async () => {
    const api = fakeFeatures([treeReply()], () => { throw new Error(); });
    await render({ features: api, lockedReason: "an experiment is active" });
    await settle();
    expect(draft("exposure").disabled).toBe(true);
    expect(writeButton("Exposure").disabled).toBe(true);
  });

  it("Feature tree shows the camera's categories and features, read-only", async () => {
    await render({ features: fakeFeatures([treeReply()], () => { throw new Error(); }) });
    await settle();
    expect($("align-feature-tree")).toBeNull();
    await press("Feature tree");
    const tree = $("align-feature-tree");
    expect($("align-feature-tree-status").textContent).toBe("8 features");
    expect(tree.textContent).toContain("▸ AcquisitionControl");
    expect(tree.textContent).toContain("ExposureTimefloatread/write1000 us10–1000000 us");
    expect(tree.textContent).toContain("DeviceModelNamestringread-onlyMIB Mock Camera");
    expect(tree.querySelectorAll("input, select")).toHaveLength(0);
  });

  it("a camera without GenApi: every SFNC row and the browser say unsupported", async () => {
    await render({ features: fakeFeatures([treeOutcome("unsupported", "The active camera has no GenApi node map.")], () => { throw new Error(); }) });
    await settle();
    expect(chip("gain").textContent).toBe("Unsupported · The active camera has no GenApi node map.");
    await press("Feature tree");
    expect($("align-feature-tree-status").textContent).toBe("unsupported · The active camera has no GenApi node map.");
  });
});
