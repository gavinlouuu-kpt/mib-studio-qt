// @vitest-environment jsdom
// R3 Inspect on the kit (#717 B3a, #269), over a mocked Tauri IPC
// (`window.__TAURI_INTERNALS__.invoke`): the selected record is named by its
// frame and object; its image views ask the backend for the right overlay
// and crop the record's box; views a record cannot show are disabled with
// the reason; Previous / Next record and Record details step and open the
// viewer; the sub-views keep the selection; the Stored card says what
// produced the stored results and sends Reprocess to the Regenerate masks
// dialog; and every feature of the panel before B3a is still reachable.
// B3c (review ABI 3): the crop is placed through the file's object
// coordinate space (ROI-local, image, unknown); the Stored card gives the
// profile and E-LUT; the linked records and the record reprocessed are sent
// only by their buttons, with every refusal said plainly.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { FramePullScheduler } from "../../framePullScheduler";
import { ReviewPanel, type ReviewPanelHandle } from "../ReviewPanel";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

type Call = { cmd: string; args: Record<string, unknown> };
const calls: Call[] = [];
let opened = "";
let masks = true;
let images = true;
let recording = false;
let canvasWorks = true;
/** The recorded ROI's origin; (0, 0) unless a test moves it. */
let roiOrigin = { x: 0, y: 0 };
/** The file's object coordinate space as fetch_review_info reports it. */
let space: { kind: string; source: string; x: number; y: number } = { kind: "image", source: "roi_origin", x: 0, y: 0 };
let lut: unknown = null;
let profile: string | null = null;
let untracked = false;
/** fetch_review_track_rows: null = the track's records from the mock rows. */
let trackAnswer: ((args: Record<string, unknown>) => unknown) | null = null;
/** review_reprocess_record: null = the stored record found again. */
let reprocessAnswer: ((args: Record<string, unknown>) => unknown) | null = null;
/** Holds review_reprocess_record's reply until released. */
let holdReprocess: { promise: Promise<void>; release: () => void } | null = null;

const W = 64, H = 48, VALID = 12, INVALID = 3;
const ds = (count: number, present = count > 0) => ({ present, count: String(count), height: H, width: W, channels: 1 });

function info() {
  const open = opened !== "";
  return {
    valid: true,
    file_open: open,
    file_path: opened,
    recording_file: recording,
    start_time_ns: "0",
    end_time_ns: "0",
    total_valid: open ? String(VALID) : "0",
    total_invalid: open && !recording ? String(INVALID) : "0",
    filtered_frames: "0",
    roi_x: roiOrigin.x,
    roi_y: roiOrigin.y,
    roi_w: W,
    roi_h: H,
    object_coordinates: open && !recording ? space.kind : "unknown",
    object_coordinates_source: open && !recording ? space.source : "",
    object_origin_x: space.x,
    object_origin_y: space.y,
    has_background: false,
    has_core_identity: open,
    core_version: "2.1.0",
    core_source: "bundled",
    core_release_tag: "",
    valid_images: ds(open && !recording && images ? VALID : 0),
    invalid_images: ds(open && !recording && images ? INVALID : 0),
    valid_masks: ds(open && !recording && masks ? VALID : 0),
    invalid_masks: ds(open && !recording && masks ? INVALID : 0),
    recorded_images: ds(open && recording ? VALID : 0),
    has_series: false,
    series_count: "0",
    multi_image_enabled: false,
    multi_image_count: "0",
    has_accounting: open,
    accounting_completion: 0,
    accounting_reconciled: true,
    accounting_empty: "0",
    accounting_rejected: "0",
    accounting_processing_failed: "0",
    accounting_store_loss: "0",
    accounting_persisted: "15",
    accounting_admitted: "15",
    accounting_persistence_failed: "0",
    accounting_summary: open ? " · run complete — empty 0" : "",
    pixel_to_micron: 0.25,
    pixel_to_micron_from_file: true,
    kde_analysis_json: "",
    kde_live_json: "",
    has_recorded_config: true,
    ring_ratio_min: 15,
    ring_ratio_max: 25,
    identity: {
      has_snapshot: open,
      snapshot_unreadable: false,
      snapshot_version: open ? 3 : null,
      run_id: open ? "run-7" : null,
      experiment_id: open ? "4f1c2a9e-0b3d-4c5e-8f7a-1b2c3d4e5f60" : null,
      run_index: open ? 2 : null,
      sample_ref: open ? "S-02" : null,
      calibration_record_id: open ? "cal-0042" : null,
      operator: null,
      annotation: null,
      annotation_unreadable: false,
      emodulus_lut: open ? lut : null,
      profile_id: open ? profile : null,
    },
  };
}

function row(valid: boolean, i: number) {
  return {
    frame_index: String((valid ? 1000 : 2000) + i * 3),
    timestamp_ns: String(i * 1000),
    // A recording's row is a frame: the backend fills the frame index and
    // timestamp and leaves the rest at its defaults (0, false).
    valid: recording ? false : valid,
    target_group: false,
    touches_border: false,
    has_single_inner_contour: true,
    in_range: true,
    in_channel: true,
    inner_contour_count: 1,
    object_id: recording ? 0 : 1 + (i % 2),
    object_count: recording ? 0 : 1,
    // Two records per track (0, 0, 1, 1, …).
    track_id: recording || untracked ? -1 : Math.floor(i / 2),
    track_first_frame: "0",
    track_last_frame: "0",
    track_observation_count: 1,
    bbox_x: recording ? 0 : 20,
    bbox_y: recording ? 0 : 10,
    bbox_width: recording ? 0 : 10,
    bbox_height: recording ? 0 : 10,
    centroid_x: 25,
    centroid_y: 15,
    area: recording ? 0 : 80 + i,
    area_um2: recording ? 0 : (80 + i) * 0.0625,
    deformability: recording ? 0 : 0.05,
    area_ratio: 1,
    ring_ratio: recording ? 0 : 20,
    laplacian_variance: 1,
    youngs_modulus: recording ? 0 : 1.5,
    brightness_q1: 1,
    brightness_q2: 2,
    brightness_q3: 3,
    brightness_q4: 4,
  };
}

// A valid review packet (kind 3, Mono8 or RGB8) of W × H.
function packet(rgb: boolean, kind = 3, w = W, h = H): ArrayBuffer {
  const bpp = rgb ? 3 : 1;
  const len = w * h * bpp;
  const b = new ArrayBuffer(96 + len);
  const d = new DataView(b);
  d.setUint32(0, 0x4642494d, true);
  d.setUint16(4, 2, true);
  d.setUint16(6, 96, true);
  d.setUint32(8, 1, true);
  d.setUint32(12, kind, true);
  d.setBigUint64(32, BigInt(w), true);
  d.setBigUint64(40, BigInt(h), true);
  d.setBigUint64(48, BigInt(rgb ? 35127316 : 17301505), true);
  d.setBigUint64(56, BigInt(w * bpp), true);
  d.setBigUint64(64, BigInt(len), true);
  return b;
}

const ok = (message = "accepted") => ({ ok: true, command: 9, message, operation_id: "0" });

function respond(cmd: string, args: Record<string, unknown>): unknown {
  switch (cmd) {
    case "review_set_pixel_to_micron":
      return null;
    case "review_open":
      opened = String(args.path);
      return ok("opened");
    case "review_close":
      opened = "";
      return ok("closed");
    case "fetch_review_info":
      return info();
    case "fetch_review_rows": {
      const valid = !!args.valid;
      const total = valid ? VALID : INVALID;
      const offset = Number(args.offset);
      const rows = [];
      for (let i = offset; i < Math.min(total, offset + Number(args.count)); i++) rows.push(row(valid, i));
      return { valid: true, total: String(total), offset: String(offset), rows };
    }
    case "fetch_review_track_rows": {
      if (trackAnswer) return trackAnswer(args);
      const valid = !!args.valid;
      const positions: string[] = [];
      const rows = [];
      for (let i = 0; i < (valid ? VALID : INVALID); i++) {
        if (Math.floor(i / 2) !== Number(args.trackId) || rows.length >= Number(args.max)) continue;
        positions.push(String(i));
        rows.push(row(valid, i));
      }
      return { ok: true, message: "", total: String(rows.length), truncated: false, positions, rows };
    }
    case "review_reprocess_record": {
      const answer = () => {
        if (reprocessAnswer) return reprocessAnswer(args);
        const stored = row(!!args.valid, Number(args.index));
        return {
          ok: true,
          message: "",
          settings: args.settings,
          used_recorded_settings: args.settings === 0,
          settings_label: args.settings === 0 ? "recorded settings (config, ROI, background); px→µm 0.2500 recorded" : "default settings (whole image, no background); px→µm 0.4886 default",
          core_version: "2.1.0",
          core_source: "bundled",
          pixel_to_micron: 0.25,
          emodulus_recomputed: false,
          stored,
          reprocessed: [{ ...stored, area: 90, bbox_x: stored.bbox_x + space.x, bbox_y: stored.bbox_y + space.y, youngs_modulus: null }],
          match_index: 0,
          object_count_differs: false,
          truncated: false,
        };
      };
      return holdReprocess ? holdReprocess.promise.then(answer) : answer();
    }
    case "fetch_review_frame":
      return images || recording ? packet(Number(args.overlay) !== 0 || !!args.roiOverlay) : new ArrayBuffer(0);
    case "fetch_review_thumbnails_packet":
    case "fetch_review_series_packet":
      return new ArrayBuffer(0);
    case "fetch_review_series_count":
      return "0";
    case "fetch_review_scatter":
      return { valid: true, pixel_to_micron: 0.25, frame_index: ["1000"], valid_position: ["0"], area_um2: [5], deformability: [0.05], target_group: [0], ring_ratio: [20] };
    case "review_request_density":
      return ok();
    case "fetch_review_density":
      return { valid: true, ready: true, levels: [1], level_count: 8, bandwidth_factor: 1, core_fraction: 0.9, computed_record_json: "" };
    case "fetch_isoelastic_curves":
      return { source: "embedded", curves: [] };
    case "poll_review_events":
      return { events: [] };
    case "review_jobs_busy":
      return false;
    case "review_list_dir":
      return [];
    case "plugin:dialog|open":
      return "/data/run.h5";
    default:
      throw new Error(`unexpected command ${cmd}`);
  }
}

class NoResizeObserver {
  observe() {}
  unobserve() {}
  disconnect() {}
}
class FakeImageData {
  constructor(
    public data: Uint8ClampedArray,
    public width: number,
    public height: number,
  ) {}
}
// The crops drawn, in order: the dirty rectangle putImageData was given.
const crops: number[][] = [];
function fakeContext(canvas: HTMLCanvasElement): CanvasRenderingContext2D {
  const state: Record<string | symbol, unknown> = {
    canvas,
    putImageData: (_d: unknown, dx: number, dy: number, x?: number, y?: number, w?: number, h?: number) => {
      if (x !== undefined) crops.push([dx, dy, x, y!, w!, h!]);
    },
  };
  return new Proxy(state, {
    get(target, prop) {
      if (prop in target) return target[prop];
      if (prop === "measureText") return (text: string) => ({ width: String(text).length * 6 });
      return () => ({ addColorStop: () => undefined });
    },
    set(target, prop, value) {
      target[prop] = value;
      return true;
    },
  }) as unknown as CanvasRenderingContext2D;
}
class FakePath2D {
  constructor() {
    return new Proxy(this, { get: (target, prop) => (prop in target ? (target as Record<string | symbol, unknown>)[prop] : () => undefined) });
  }
}

let root: Root;
let host: HTMLDivElement;
const panel = { current: null as ReviewPanelHandle | null };

beforeEach(() => {
  vi.useFakeTimers();
  calls.length = 0;
  crops.length = 0;
  opened = "";
  masks = true;
  images = true;
  recording = false;
  canvasWorks = true;
  roiOrigin = { x: 0, y: 0 };
  space = { kind: "image", source: "roi_origin", x: 0, y: 0 };
  lut = null;
  profile = null;
  untracked = false;
  trackAnswer = null;
  reprocessAnswer = null;
  holdReprocess = null;
  const saved = new Map<string, string>();
  vi.stubGlobal("localStorage", {
    getItem: (k: string) => saved.get(k) ?? null,
    setItem: (k: string, v: string) => void saved.set(k, String(v)),
    removeItem: (k: string) => void saved.delete(k),
    clear: () => saved.clear(),
  });
  Object.assign(globalThis, { ResizeObserver: NoResizeObserver, Path2D: FakePath2D, ImageData: FakeImageData });
  HTMLCanvasElement.prototype.getContext = function (this: HTMLCanvasElement) {
    return canvasWorks ? fakeContext(this) : null;
  } as unknown as typeof HTMLCanvasElement.prototype.getContext;
  HTMLCanvasElement.prototype.toDataURL = function (this: HTMLCanvasElement) {
    return `data:image/png;base64,CROP${this.width}x${this.height}`;
  };
  (window as unknown as { __TAURI_INTERNALS__: unknown }).__TAURI_INTERNALS__ = {
    invoke: async (cmd: string, args: Record<string, unknown>) => {
      calls.push({ cmd, args: args ?? {} });
      return respond(cmd, args ?? {});
    },
    transformCallback: () => 0,
  };
  host = document.createElement("div");
  document.body.append(host);
  root = createRoot(host);
});

afterEach(async () => {
  await act(async () => root.unmount());
  host.remove();
  vi.useRealTimers();
  vi.unstubAllGlobals();
});

async function settle(ms = 0) {
  for (let i = 0; i < 5; i++) await act(async () => void (await vi.advanceTimersByTimeAsync(ms)));
}

function buttons(scope: ParentNode = host): HTMLButtonElement[] {
  return Array.from(scope.querySelectorAll("button"));
}
function button(text: string, scope: ParentNode = host): HTMLButtonElement {
  const found = buttons(scope).filter((b) => b.textContent?.trim() === text);
  if (found.length === 0) throw new Error(`no button "${text}" in: ${buttons(scope).map((b) => b.textContent?.trim()).join(" | ")}`);
  return found[0];
}
async function click(el: Element, ms = 0) {
  await act(async () => void el.dispatchEvent(new MouseEvent("click", { bubbles: true })));
  await settle(ms);
}
async function dblclick(el: Element) {
  await act(async () => void el.dispatchEvent(new MouseEvent("dblclick", { bubbles: true })));
  await settle();
}
const card = (title: string) => {
  const el = Array.from(host.querySelectorAll(".mib-card")).find((c) => c.querySelector(".mib-card__title")?.textContent === title);
  if (!el) throw new Error(`no card ${title}`);
  return el as HTMLElement;
};
const identityText = () => host.querySelector('[data-testid="record-identity"]')?.textContent ?? "";
const recordName = () => host.querySelector('[data-testid="record-identity"] .mib-record__name')?.textContent ?? "";
const lastFrame = () => [...calls].reverse().find((c) => c.cmd === "fetch_review_frame")?.args;
function factValue(scope: ParentNode, label: string): string | undefined {
  const dt = Array.from(scope.querySelectorAll("dt")).find((d) => d.textContent === label);
  return dt?.nextElementSibling?.textContent ?? undefined;
}

async function mountOpen(path = "/data/run.h5") {
  const scheduler = new FramePullScheduler();
  await act(async () =>
    root.render(<ReviewPanel ref={(h) => void (panel.current = h)} ready={true} scheduler={scheduler} fitWindow={true} log={() => undefined} fallbackPixelToMicron={0.4886} />),
  );
  await settle();
  if (path) {
    await act(async () => void (await panel.current!.openPath(path)));
    await settle();
  }
}

it("says no run is open, with a way to open one", async () => {
  await mountOpen("");
  const notice = host.querySelector(".mib-notice")!;
  expect(notice.textContent).toContain("No run open");
  expect(buttons(notice).map((b) => b.textContent)).toEqual(["Select HDF File…"]);
  expect(host.querySelector(".rv-record-card")).toBeNull();
});

it("names the selected record by its frame and object and places it in its run", async () => {
  await mountOpen();
  expect(recordName()).toBe("frame 1000 · object 1");
  expect(identityText()).toContain("valid record 1 of 12 · run 2 · sample S-02");
  expect(identityText()).toContain("experiment 4f1c2a9e… · snapshot v3 · calibration cal-0042");
  expect(identityText()).not.toMatch(/R-\d/);
  const record = card("Selected measurement");
  expect(factValue(record, "Area")).toBe("5.00 µm² · 80.0 px²");
  expect(factValue(record, "Track")).toContain("track 0");
  expect(factValue(record, "Mask / contour")).toBe("Saved");
  // Next / Previous record step through the set; the ends are disabled.
  expect(button("Previous record").disabled).toBe(true);
  await click(button("Next record"));
  expect(recordName()).toBe("frame 1003 · object 2");
  await click(button("Previous record"));
  expect(recordName()).toBe("frame 1000 · object 1");
});

it("asks the backend for each image view's overlay and crops the record's box", async () => {
  await mountOpen();
  // Full frame first: the toolbar's overlay and ROI, as the preview always did.
  expect(lastFrame()).toEqual({ dataset: 0, index: "0", overlay: 0, roiOverlay: false });
  const overlay = host.querySelector('select[aria-label="Overlay mode"]') as HTMLSelectElement;
  await act(async () => {
    overlay.value = "2";
    overlay.dispatchEvent(new Event("change", { bubbles: true }));
  });
  await settle();
  expect(lastFrame()).toMatchObject({ overlay: 2, roiOverlay: false });
  const roi = Array.from(host.querySelectorAll("label")).find((l) => l.textContent?.trim() === "ROI")!.querySelector("input")!;
  await click(roi);
  expect(lastFrame()).toEqual({ dataset: 0, index: "0", overlay: 2, roiOverlay: true });

  for (const [view, mode] of [["Contour", 1], ["Mask", 3], ["Raw", 0]] as const) {
    crops.length = 0;
    await click(button(view));
    expect(button(view).getAttribute("aria-pressed")).toBe("true");
    // A crop never carries the ROI, whatever the toolbar says.
    expect(lastFrame(), view).toEqual({ dataset: 0, index: "0", overlay: mode, roiOverlay: false });
    // bbox 20,10 10×10 → a 24 px square centred on (25, 15): (13, 3).
    expect(crops, view).toEqual([[-13, -3, 13, 3, 24, 24]]);
    const img = card("Selected measurement").querySelector(".mib-cell__img") as HTMLImageElement;
    expect(img.getAttribute("src"), view).toBe("data:image/png;base64,CROP24x24");
    expect(card("Selected measurement").querySelector(".mib-cell__label")?.textContent).toBe(view.toUpperCase());
    expect((host.querySelector(".canvas-wrap.preview") as HTMLElement).hidden).toBe(true);
  }
  // The crop follows the selection.
  await click(button("Next record"));
  expect(lastFrame()).toEqual({ dataset: 0, index: "1", overlay: 0, roiOverlay: false });
  await click(button("Full frame"));
  expect(lastFrame()).toEqual({ dataset: 0, index: "1", overlay: 2, roiOverlay: true });
  expect((host.querySelector(".canvas-wrap.preview") as HTMLElement).hidden).toBe(false);
});

it("places the crop through the file's object coordinate space, also with a moved ROI (B3c)", async () => {
  // A capture whose ROI starts at (16, 8) stores ROI-local boxes next to
  // full-frame images: the crop is cut at box + origin.
  roiOrigin = { x: 16, y: 8 };
  space = { kind: "roi_local", source: "attribute", x: 16, y: 8 };
  await mountOpen("/data/roi.h5");
  await click(button("Raw"));
  // bbox 20,10 10×10 + (16, 8) = 36,18 → a 24 px square centred on (41, 23): (29, 11).
  expect(crops).toEqual([[-29, -11, 29, 11, 24, 24]]);
  expect(card("Selected measurement").querySelector(".rv-crop-note")).toBeNull();
  expect(factValue(card("Stored vs reprocessed"), "Object coordinates")).toBe(
    "ROI-local, placed at the ROI origin (16, 8) · recorded in the file (@object_coordinates)",
  );
  // The ROI moved to (30, 20) in another capture: the crop moves with it.
  roiOrigin = { x: 30, y: 20 };
  space = { kind: "roi_local", source: "snapshot", x: 30, y: 20 };
  crops.length = 0;
  await act(async () => void (await panel.current!.openPath("/data/roi-moved.h5")));
  await settle();
  await click(button("Contour"));
  // 50,30 → centre (55, 35) → (43, 23), inside the 64 × 48 frame.
  expect(crops[crops.length - 1]).toEqual([-40, -23, 40, 23, 24, 24]);
  // A regenerated file stores image coordinates: as stored, whatever its ROI.
  space = { kind: "image", source: "attribute", x: 0, y: 0 };
  crops.length = 0;
  await act(async () => void (await panel.current!.openPath("/data/regenerated.h5")));
  await settle();
  await click(button("Raw"));
  expect(crops[crops.length - 1]).toEqual([-13, -3, 13, 3, 24, 24]);
  expect(factValue(card("Stored vs reprocessed"), "Object coordinates")).toBe("image · recorded in the file (@object_coordinates)");
});

it("offers no cell crop when nothing decides the file's space, and says why", async () => {
  await mountOpen();
  await click(button("Raw"));
  expect(button("Raw").getAttribute("aria-pressed")).toBe("true");
  roiOrigin = { x: 4, y: 2 };
  space = { kind: "unknown", source: "", x: 0, y: 0 };
  crops.length = 0;
  await act(async () => void (await panel.current!.openPath("/data/legacy.h5")));
  await settle();
  const measured = card("Selected measurement");
  const why = "No cell crop: the file does not record whether its bounding boxes are relative to its ROI or to the image, and its masks do not tell";
  for (const view of ["Raw", "Contour", "Mask"]) {
    expect(button(view).disabled, view).toBe(true);
    expect(button(view).title, view).toBe(why);
  }
  expect(button("Full frame").getAttribute("aria-pressed")).toBe("true");
  expect(measured.querySelector(".rv-crop-note")?.textContent).toBe(why);
  expect(measured.querySelector(".mib-cell__img")).toBeNull();
  expect(crops).toEqual([]);
  expect(lastFrame()).toEqual({ dataset: 0, index: "0", overlay: 0, roiOverlay: false });
  await click(button("Next record"));
  expect(crops).toEqual([]);
  expect(factValue(card("Stored vs reprocessed"), "Object coordinates")).toMatch(/^Unknown: the file does not record it/);
  // A file whose ROI origin is not (0, 0) but whose space is decided offers
  // the crop (the B3a stopgap is gone).
  space = { kind: "image", source: "mask_check", x: 0, y: 0 };
  await act(async () => void (await panel.current!.openPath("/data/decided.h5")));
  await settle();
  expect(button("Raw").disabled).toBe(false);
  expect(card("Selected measurement").querySelector(".rv-crop-note")).toBeNull();
});

it("says so when a crop cannot be drawn, and when the image was not recorded", async () => {
  canvasWorks = false;
  await mountOpen();
  await click(button("Raw"));
  expect(card("Selected measurement").textContent).toContain("The crop could not be drawn");
  expect(card("Selected measurement").textContent).not.toContain("Image not recorded");
  // A file without images: the kit's CellImage says the image was not recorded.
  canvasWorks = true;
  images = false;
  await act(async () => void (await panel.current!.openPath("/data/noimages.h5")));
  await settle();
  // Its records are still selected and stepped through (rows, not images).
  expect(recordName()).toBe("frame 1000 · object 1");
  expect(identityText()).toContain("valid record 1 of 12");
  await click(button("Next record"));
  expect(recordName()).toBe("frame 1003 · object 2");
  await click(button("Raw"));
  expect(card("Selected measurement").textContent).toContain("Image not recorded");
  expect(factValue(card("Selected measurement"), "Full frame")).toBe("Not recorded");
});

it("disables the views a record cannot show, with the reason", async () => {
  masks = false;
  await mountOpen();
  expect(button("Raw").disabled).toBe(false);
  for (const view of ["Contour", "Mask"]) {
    expect(button(view).disabled, view).toBe(true);
    expect(button(view).title).toBe("No masks are recorded in this set");
  }
  expect(factValue(card("Selected measurement"), "Mask / contour")).toBe("Not recorded");
  // A raw recording: no objects to crop, the full frame only.
  recording = true;
  await act(async () => void (await panel.current!.openPath("/data/rec.h5")));
  await settle();
  for (const view of ["Raw", "Contour", "Mask"]) expect(button(view).title, view).toBe("Recording files carry no objects");
  expect(button("Full frame").disabled).toBe(false);
  expect(button("Full frame").getAttribute("aria-pressed")).toBe("true");
  expect(buttons().map((b) => b.textContent)).not.toContain("Scatter");
  expect(buttons().map((b) => b.textContent)).toContain("Frames (12)");
});

it("shows a raw recording's frame and time only, never its unfilled values (#757 review)", async () => {
  recording = true;
  await mountOpen();
  await act(async () => void (await panel.current!.openPath("/data/rec.h5")));
  await settle();
  const measured = card("Selected measurement");
  // A recording's rows are frames: the name has no object.
  expect(recordName()).toBe("frame 1000");
  expect(factValue(measured, "Frame")).toBe("1000");
  expect(factValue(measured, "Timestamp")).toBe("0 ns");
  // The backend leaves the measurement fields of a recording's row at 0 and
  // its validation false: none of that is shown as a value.
  for (const label of ["Area", "Deformability", "E (stored)", "Ring ratio", "Validation", "Track", "Objects in frame"]) {
    expect(factValue(measured, label), label).toBe("Not recorded");
  }
  expect(factValue(measured, "Analysis")).toBe("None (raw recording)");
  const facts = Array.from(measured.querySelectorAll(".rv-record__body dd")).map((d) => d.textContent ?? "");
  expect(facts.join(" | ")).not.toMatch(/\binvalid\b|µm²|px²|kPa|\bobject\b/);
  await click(button("Next record"));
  expect(recordName()).toBe("frame 1003");
  expect(factValue(card("Selected measurement"), "Area")).toBe("Not recorded");
});

it("opens the viewer from Record details, the full frame and a table row", async () => {
  await mountOpen();
  await click(button("Record details"));
  expect(host.querySelector('[role="dialog"][aria-label="Frame viewer"]')).not.toBeNull();
  await act(async () => void window.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape" })));
  await settle();
  expect(host.querySelector('[role="dialog"][aria-label="Frame viewer"]')).toBeNull();
  await dblclick(host.querySelector(".canvas-wrap.preview")!);
  expect(host.querySelector('[role="dialog"][aria-label="Frame viewer"]')).not.toBeNull();
  await act(async () => void window.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape" })));
  await settle();
  // Records: a click selects the row, a double-click opens it.
  await click(button("Records"));
  expect(host.querySelector('[role="listbox"]')).toBeNull();
  const third = host.querySelectorAll(".metrics-table tbody tr")[2];
  await click(third);
  expect(recordName()).toBe("frame 1006 · object 1");
  expect(third.getAttribute("aria-current")).toBe("true");
  await dblclick(third.querySelector("td")!);
  expect(host.querySelector('[role="dialog"][aria-label="Frame viewer"]')?.textContent).toContain("1006");
});

it("keeps the selection across sub-views; the scatter shows the valid set", async () => {
  await mountOpen();
  await click(button("Next record"));
  await click(button("Records"));
  expect(recordName()).toBe("frame 1003 · object 2");
  await click(button("Scatter"));
  expect(calls.some((c) => c.cmd === "fetch_review_scatter")).toBe(true);
  expect(recordName()).toBe("frame 1003 · object 2");
  expect(host.querySelector(".rv-scatter-card")).not.toBeNull();
  // The invalid set has no scatter: it shows its frames, from its first record.
  await click(button(`Invalid Frames (${INVALID})`));
  expect(button("Frames").getAttribute("aria-pressed")).toBe("true");
  expect(recordName()).toBe("frame 2000 · object 1");
  expect(identityText()).toContain("invalid record 1 of 3");
  // Back to the scatter: the valid set, nothing selected until a point is clicked.
  calls.length = 0;
  await click(button("Scatter"));
  expect(calls.find((c) => c.cmd === "fetch_review_rows")?.args).toEqual({ valid: true, offset: "0", count: 100 });
  expect(button(`Valid Frames (${VALID})`).getAttribute("aria-pressed")).toBe("true");
  expect(card("Selected measurement").textContent).toContain("Click a point to inspect its record");
  expect(button("Record details").disabled).toBe(true);
});

it("says what produced the stored results and sends Reprocess to Regenerate masks", async () => {
  await mountOpen();
  const stored = card("Stored vs reprocessed");
  expect(factValue(stored, "Stored results")).toBe("core 2.1.0 (bundled) recorded in this file");
  expect(factValue(stored, "Calibration used")).toBe("cal-0042 · px→µm 0.2500 (recorded)");
  // A v3 snapshot without an E-LUT or profile: "Not recorded", never a value.
  expect(factValue(stored, "E-LUT")).toBe("Not recorded");
  expect(factValue(stored, "Profile")).toBe("Not recorded");
  expect(identityText()).toContain("profile not recorded · E-LUT not recorded");
  expect(factValue(stored, "Reprocessing")).toBe("Reprocessing writes a new file; this view lists no history");
  expect(stored.textContent).not.toContain("Compare");
  await click(button("Reprocess into a new file…", stored));
  expect(host.querySelector('[role="dialog"][aria-label="Regenerate masks"]')).not.toBeNull();
  expect(calls.some((c) => c.cmd === "review_reprocess_record")).toBe(false);
});

it("gives the run's E-LUT and profile as recorded (review ABI 3)", async () => {
  lut = { loaded: true, lut_id: "emod-lut-2026a", revision: "3", source: "catalog", sha256: "9f86d081884c7d659a2feaa0c55ad015", catalog_sha256: null, checksum_status: "verified" };
  profile = "rbc-default";
  await mountOpen();
  const stored = card("Stored vs reprocessed");
  expect(factValue(stored, "E-LUT")).toBe("emod-lut-2026a · revision 3 · catalog · sha256 9f86d081884c… · checksum verified");
  expect(factValue(stored, "Profile")).toBe("rbc-default");
  expect(identityText()).toContain("calibration cal-0042 · profile rbc-default · E-LUT emod-lut-2026a rev 3");
  expect(stored.textContent).not.toContain("Not available");
  // A run that loaded no LUT says so.
  lut = { loaded: false, lut_id: null, revision: null, source: null, sha256: null, catalog_sha256: null, checksum_status: null };
  await act(async () => void (await panel.current!.openPath("/data/nolut.h5")));
  await settle();
  expect(factValue(card("Stored vs reprocessed"), "E-LUT")).toBe("none loaded (the run had no E values)");
});

it("reads the selected record's linked records only when asked, and says when the cap cut them", async () => {
  await mountOpen();
  const linked = () => card("Linked cell records");
  const trackCalls = () => calls.filter((c) => c.cmd === "fetch_review_track_rows");
  expect(linked().textContent).toContain("Track 0 · 1 observation recorded");
  expect(trackCalls()).toEqual([]);
  await click(button("Show linked records", linked()));
  expect(trackCalls().map((c) => c.args)).toEqual([{ valid: true, trackId: 0, max: 256 }]);
  expect(linked().querySelector('[data-testid="track-summary"]')?.textContent).toBe("Track 0: 2 records in the valid set");
  const rows = linked().querySelectorAll("tbody tr");
  expect(Array.from(rows).map((r) => r.querySelector("td:nth-child(2)")?.textContent)).toEqual(["1000", "1003"]);
  expect(rows[0].getAttribute("aria-current")).toBe("true");
  // A row selects its record; the same track stays shown, with no new call.
  await click(rows[1]);
  expect(recordName()).toBe("frame 1003 · object 2");
  expect(linked().querySelector('[data-testid="track-summary"]')).not.toBeNull();
  expect(trackCalls()).toHaveLength(1);
  // The next record is on another track: nothing is read until asked.
  await click(button("Next record"));
  expect(recordName()).toBe("frame 1006 · object 1");
  expect(linked().querySelector('[data-testid="track-summary"]')).toBeNull();
  expect(trackCalls()).toHaveLength(1);
  // A track longer than the cap: the first 256, and said so.
  trackAnswer = (args) => ({
    ok: true,
    message: "",
    total: "300",
    truncated: true,
    positions: Array.from({ length: Number(args.max) }, (_, k) => String(k)),
    rows: Array.from({ length: Number(args.max) }, (_, k) => row(true, k % VALID)),
  });
  await click(button("Show linked records", linked()));
  const summary = linked().querySelector('[data-testid="track-summary"]')!;
  expect(summary.textContent).toBe("Track 1: 300 records in the valid set; the first 256 are shown (at most 256 per request)");
  expect(summary.className).toContain("rv-truncated");
  expect(linked().querySelectorAll("tbody tr")).toHaveLength(256);
  // A refusal is said plainly.
  trackAnswer = () => ({ ok: false, message: "No review file is open", total: "0", truncated: false, positions: [], rows: [] });
  await click(button("Next record"));
  await click(button("Next record"));
  await click(button("Show linked records", linked()));
  expect(linked().querySelector('[role="alert"]')?.textContent).toBe("Not read: No review file is open");
});

it("offers no linked records for an untracked record or a recording", async () => {
  untracked = true;
  await mountOpen();
  expect(button("Show linked records", card("Linked cell records")).disabled).toBe(true);
  expect(card("Linked cell records").textContent).toContain("Not tracked: this record has no linked records");
  recording = true;
  await act(async () => void (await panel.current!.openPath("/data/rec.h5")));
  await settle();
  expect(host.querySelector(".rv-track-card")).toBeNull();
  expect(calls.some((c) => c.cmd === "fetch_review_track_rows")).toBe(false);
});

it("reprocesses the selected record only on its button and shows stored next to reprocessed", async () => {
  roiOrigin = { x: 16, y: 8 };
  space = { kind: "roi_local", source: "attribute", x: 16, y: 8 };
  await mountOpen();
  const reprocessCalls = () => calls.filter((c) => c.cmd === "review_reprocess_record");
  const stored = () => card("Stored vs reprocessed");
  // Opening, stepping, sub-views and the set send nothing.
  await click(button("Next record"));
  await click(button("Records"));
  await click(button("Frames"));
  await click(button(`Invalid Frames (${INVALID})`));
  await click(button(`Valid Frames (${VALID})`));
  expect(reprocessCalls()).toEqual([]);
  await click(button("Reprocess this record", stored()));
  expect(reprocessCalls().map((c) => c.args)).toEqual([{ valid: true, index: "0", settings: 0 }]);
  const result = stored().querySelector('[data-testid="reprocess-result"]')!;
  expect(result.querySelector('[data-testid="reprocess-match"]')?.textContent).toBe(
    "Matched object 1 of 1 object found (centroid nearest the stored one, inside the stored box)",
  );
  const cells = (label: string) => {
    const tr = Array.from(result.querySelectorAll("tbody tr")).find((r) => r.querySelector("td")?.textContent === label);
    return Array.from(tr?.querySelectorAll("td") ?? []).slice(1).map((td) => td.textContent);
  };
  expect(cells("Area (px²)")).toEqual(["80.0", "90.0"]);
  // The stored ROI-local box is shown placed in the image, beside the found one.
  expect(cells("Box (image x, y · w×h)")).toEqual(["36, 18 · 10×10", "36, 18 · 10×10"]);
  expect(cells("E")).toEqual(["1.50 kPa", "not recomputed (no E-LUT loaded)"]);
  expect(result.textContent).toContain("Settings: recorded settings (config, ROI, background); px→µm 0.2500 recorded · core 2.1.0 (bundled)");
  expect(result.textContent).toContain("nothing is written");
  // Moving on hides it and sends nothing; default settings go as 1.
  await click(button("Next record"));
  expect(stored().querySelector('[data-testid="reprocess-result"]')).toBeNull();
  expect(reprocessCalls()).toHaveLength(1);
  const settings = stored().querySelector('select[aria-label="Reprocess settings"]') as HTMLSelectElement;
  await act(async () => {
    settings.value = "1";
    settings.dispatchEvent(new Event("change", { bubbles: true }));
  });
  await settle();
  expect(reprocessCalls()).toHaveLength(1);
  await click(button("Reprocess this record", stored()));
  expect(reprocessCalls()[reprocessCalls().length - 1]?.args).toEqual({ valid: true, index: "1", settings: 1 });
  expect(stored().textContent).toContain("default settings (whole image, no background)");
  // "Reprocess into a new file…" and the #754 wording stay.
  expect(factValue(stored(), "Reprocessing")).toBe("Reprocessing writes a new file; this view lists no history");
  expect(button("Reprocess into a new file…", stored()).disabled).toBe(false);
});

it("says every reprocess refusal plainly", async () => {
  await mountOpen();
  const stored = () => card("Stored vs reprocessed");
  const refusal = () => stored().querySelector('[role="alert"]')?.textContent;
  const refuse = (message: string) => () => ({ ok: false, message, settings: 0, stored: null, reprocessed: [], match_index: null });
  // The processing lane claimed (a job started elsewhere after the card was drawn).
  reprocessAnswer = refuse("A review job is running; reprocess the record when it finishes");
  await click(button("Reprocess this record", stored()));
  expect(refusal()).toBe("Not reprocessed: A review job is running; reprocess the record when it finishes");
  reprocessAnswer = refuse("Record 0 has no stored image to reprocess");
  await click(button("Reprocess this record", stored()));
  expect(refusal()).toBe("Not reprocessed: Record 0 has no stored image to reprocess");
  // The backend refuses an unknown space for the whole call.
  reprocessAnswer = refuse("Record 0 cannot be matched: the file does not record whether its bounding boxes are relative to its ROI (which starts at 4, 2) or to the image, and its masks do not tell");
  await click(button("Reprocess this record", stored()));
  expect(refusal()).toContain("Not reprocessed: Record 0 cannot be matched");
  // No match: answered, but no found object is the stored one.
  reprocessAnswer = (args) => ({
    ok: true,
    message: "",
    settings: args.settings,
    used_recorded_settings: true,
    settings_label: "recorded settings (config, ROI, background)",
    core_version: "2.1.0",
    core_source: "bundled",
    pixel_to_micron: 0.25,
    emodulus_recomputed: false,
    stored: row(true, 0),
    reprocessed: [{ ...row(true, 0), centroid_x: 60, centroid_y: 40 }],
    match_index: null,
    object_count_differs: true,
    truncated: false,
  });
  await click(button("Reprocess this record", stored()));
  expect(refusal()).toBeUndefined();
  expect(stored().querySelector('[data-testid="reprocess-match"]')?.textContent).toBe(
    "No match: the kernel found 1 object, none with its centroid inside the stored box",
  );
  const result = stored().querySelector('[data-testid="reprocess-result"]')!;
  const area = Array.from(result.querySelectorAll("tbody tr")).find((r) => r.querySelector("td")?.textContent === "Area (px²)")!;
  expect(Array.from(area.querySelectorAll("td")).map((td) => td.textContent)).toEqual(["Area (px²)", "80.0", "–"]);
  // A file whose space nothing decides: the button says why, and nothing is sent.
  space = { kind: "unknown", source: "", x: 0, y: 0 };
  calls.length = 0;
  await act(async () => void (await panel.current!.openPath("/data/legacy.h5")));
  await settle();
  const button_ = button("Reprocess this record", stored());
  expect(button_.disabled).toBe(true);
  expect(stored().querySelector(".rv-reprocess-unavailable")?.textContent).toMatch(
    /^Not offered: the file does not record whether its bounding boxes are relative to its ROI or to the image/,
  );
  await click(button_);
  expect(calls.some((c) => c.cmd === "review_reprocess_record")).toBe(false);
  // A recording: no objects, no button to press.
  recording = true;
  await act(async () => void (await panel.current!.openPath("/data/rec.h5")));
  await settle();
  expect(button("Reprocess this record", stored()).disabled).toBe(true);
  expect(button("Reprocess this record", stored()).title).toBe("Recording files carry no objects");
});

it("drops a reprocess reply that lands after the operator moved on", async () => {
  await mountOpen();
  let release = () => undefined as void;
  holdReprocess = { promise: new Promise<void>((r) => (release = r)), release: () => release() };
  await click(button("Reprocess this record", card("Stored vs reprocessed")));
  expect(card("Stored vs reprocessed").textContent).toContain("Reprocessing the record…");
  expect(button("Reprocess this record", card("Stored vs reprocessed")).disabled).toBe(true);
  await click(button("Next record"));
  holdReprocess.release();
  await settle();
  expect(recordName()).toBe("frame 1003 · object 2");
  expect(card("Stored vs reprocessed").querySelector('[data-testid="reprocess-result"]')).toBeNull();
  expect(card("Stored vs reprocessed").textContent).not.toContain("Reprocessing the record…");
  // Back on the first record, the dropped reply is not shown either, and
  // nothing is left pending: the button can be pressed again.
  await click(button("Previous record"));
  expect(card("Stored vs reprocessed").querySelector('[data-testid="reprocess-result"]')).toBeNull();
  expect(card("Stored vs reprocessed").textContent).not.toContain("Reprocessing the record…");
  expect(button("Reprocess this record", card("Stored vs reprocessed")).disabled).toBe(false);
  holdReprocess = null;
  await click(button("Reprocess this record", card("Stored vs reprocessed")));
  expect(card("Stored vs reprocessed").querySelector('[data-testid="reprocess-result"]')).not.toBeNull();
});

it("drops a track reply that lands after the operator moved to another track", async () => {
  await mountOpen();
  let release = () => undefined as void;
  const held = new Promise<void>((r) => (release = r));
  trackAnswer = (args) => held.then(() => ({ ok: true, message: "", total: "1", truncated: false, positions: ["0"], rows: [row(true, Number(args.trackId) * 2)] }));
  await click(button("Show linked records", card("Linked cell records")));
  expect(card("Linked cell records").textContent).toContain("Reading the track…");
  // Records 0 and 1 share track 0; record 2 is on track 1.
  await click(button("Next record"));
  await click(button("Next record"));
  release();
  await settle();
  await click(button("Previous record"));
  await click(button("Previous record"));
  expect(card("Linked cell records").querySelector('[data-testid="track-summary"]')).toBeNull();
  expect(card("Linked cell records").textContent).not.toContain("Reading the track…");
  expect(button("Show linked records", card("Linked cell records")).disabled).toBe(false);
});

it("sends nothing new on selection, sub-view or set changes (B3a's calls only)", async () => {
  await mountOpen();
  const sent = () => calls.map((c) => c.cmd);
  calls.length = 0;
  await click(button("Next record"));
  await click(button("Previous record"));
  expect(sent()).toEqual(["fetch_review_frame", "fetch_review_frame"]);
  calls.length = 0;
  await click(button("Records"));
  await click(button("Frames"));
  expect(sent().filter((c) => c !== "fetch_review_thumbnails_packet")).toEqual([]);
  calls.length = 0;
  await click(button(`Invalid Frames (${INVALID})`));
  expect(new Set(sent())).toEqual(new Set(["fetch_review_rows", "fetch_review_thumbnails_packet", "fetch_review_frame"]));
  calls.length = 0;
  await click(button("Raw"));
  expect(sent()).toEqual(["fetch_review_frame"]);
  for (const c of ["fetch_review_track_rows", "review_reprocess_record"]) expect(calls.some((x) => x.cmd === c)).toBe(false);
});

it("keeps every feature of the panel before B3a reachable", async () => {
  await mountOpen();
  const names = () => buttons().map((b) => b.textContent?.trim());
  for (const name of ["Select HDF File…", "Close File", "Export Metrics to CSV…", "Export All…", "More…", "◀", "▶", "Columns…", `Valid Frames (${VALID})`, `Invalid Frames (${INVALID})`, "Scatter", "Records", "Frames"])
    expect(names(), name).toContain(name);
  await click(button("More…"));
  for (const name of ["Batch Metrics…", "Batch Export All…", "Export Charts…", "Regenerate masks…"]) expect(names(), name).toContain(name);
  expect(host.querySelector('select[aria-label="Overlay mode"]')?.querySelectorAll("option").length).toBe(5);
  expect(Array.from(host.querySelectorAll("label")).some((l) => l.textContent?.trim() === "ROI")).toBe(true);
  // Frames: the thumbnail grid (double-click / Enter opens the viewer).
  expect(host.querySelector('[role="listbox"][aria-label="Frames"]')).not.toBeNull();
  // The column chooser.
  await click(button("Columns…"));
  expect(host.querySelector(".column-popup")).not.toBeNull();
  // Scatter: the chart's own controls.
  await click(button("Scatter"));
  for (const name of ["Compute core contour…", "Reset zoom"]) expect(names(), name).toContain(name);
  expect(host.textContent).toContain("Density colours");
  expect(host.textContent).toContain("Isoelastic curves");
  // Close.
  await click(button("Close File"));
  expect(host.querySelector(".mib-notice")?.textContent).toContain("No run open");
});
