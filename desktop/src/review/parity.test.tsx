// @vitest-environment jsdom
// YOFO Review against the Qt Review tab, the minor rows of #867 (review area),
// driven through the whole panel over a minimal fake backend at
// `window.__TAURI_INTERNALS__.invoke` (the harness of reviewIpc.test.tsx):
// thumbnail tooltip (REVIEW-014), Valid / Invalid tabs keeping their selection
// and scroll (018), the viewer sharing overlay and ROI with the toolbar (028),
// Export as TIFF (031), the Charts frame pane (032), the core share
// preference (036/037), the draggable, remembered splits (039), and the
// "Show in folder" failure that used to be swallowed.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { FramePullScheduler } from "../framePullScheduler";
import { ReviewPanel } from "./ReviewPanel";
import ReviewApp from "./ReviewApp";
import { JobDialog } from "./exports/ExportDialogs";
import { OPERATION_STATES } from "../bridgeContract";
import { REVIEW_OPERATION_KINDS } from "./reviewContract";
import { CORE_FRACTION_KEY } from "./charts/chartData";
import { CHARTS_RIGHT_SPLITTER_KEY, CHARTS_SPLITTER_KEY } from "./charts/splitter";

vi.mock("@tauri-apps/api/app", () => ({ getVersion: vi.fn(async () => "0.0.0") }));
vi.mock("@tauri-apps/api/event", () => ({ listen: vi.fn(async () => () => undefined) }));

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

const last = <T,>(items: T[]): T | undefined => items[items.length - 1];

type Call = { cmd: string; args: Record<string, unknown>; options?: unknown };
const calls: Call[] = [];
let opened = "";
let nextOperation = 100;
const queuedEvents: unknown[] = [];
// What the fake operator answers / what fails.
const answers = { save: null as string | null, revealFails: false };

const dataset = (count: number) => ({ present: count > 0, count: String(count), height: 96, width: 512, channels: 1 });

function info() {
  const open = opened !== "";
  return {
    valid: true, file_open: open, file_path: opened, recording_file: false, start_time_ns: "0", end_time_ns: "1000",
    total_valid: open ? "250" : "0", total_invalid: open ? "2" : "0", filtered_frames: "0",
    roi_x: 0, roi_y: 0, roi_w: 512, roi_h: 96, has_background: true, has_core_identity: true, core_version: "1.2.0",
    core_source: "bundled", core_release_tag: "",
    valid_images: dataset(open ? 250 : 0), invalid_images: dataset(open ? 2 : 0), valid_masks: dataset(open ? 250 : 0),
    invalid_masks: dataset(open ? 2 : 0), recorded_images: dataset(0),
    has_series: open, series_count: open ? "4" : "0", multi_image_enabled: false, multi_image_count: "0",
    has_accounting: false, accounting_completion: 0, accounting_reconciled: false, accounting_empty: "0",
    accounting_rejected: "0", accounting_processing_failed: "0", accounting_store_loss: "0", accounting_persisted: "0",
    accounting_admitted: "0", accounting_persistence_failed: "0", accounting_summary: "",
    pixel_to_micron: 0.5, pixel_to_micron_from_file: false, kde_analysis_json: "", kde_live_json: "",
    has_recorded_config: true, ring_ratio_min: 15, ring_ratio_max: 25,
  };
}

function row(i: number) {
  return {
    frame_index: String(i * 3), timestamp_ns: String(i * 1000), valid: true, target_group: false, touches_border: false,
    has_single_inner_contour: true, in_range: true, in_channel: true, inner_contour_count: 1, object_id: 0, object_count: 1,
    track_id: i, track_first_frame: String(i * 3), track_last_frame: String(i * 3), track_observation_count: 1,
    bbox_x: 1, bbox_y: 2, bbox_width: 30, bbox_height: 20, centroid_x: 16, centroid_y: 12, area: 400 + i, area_um2: 100 + i,
    deformability: 0.01 * (i + 1), area_ratio: 1.01, ring_ratio: 18, laplacian_variance: null, youngs_modulus: 1.5,
    brightness_q1: 10, brightness_q2: 20, brightness_q3: 30, brightness_q4: 40,
  };
}

const COMPUTED_RECORD = JSON.stringify({ contours: [[[11, 0.011], [21, 0.021], [31, 0.011]]], core_fraction: 0.7, provisional: false, source: "full_run", cell_count: 2, population_count: 3 });
const accepted = (id = "0") => ({ ok: true, command: 0, message: "accepted", operation_id: id });

function job(kind: number, message: string) {
  const id = String(nextOperation++);
  queuedEvents.push({ operation_id: id, kind, state: OPERATION_STATES.Completed, progress: "2", total: "2", message });
  return accepted(id);
}

function respond(cmd: string, args: Record<string, unknown>): unknown {
  switch (cmd) {
    case "is_initialized": return true;
    case "init": return true;
    case "shell_log": return null;
    case "review_abi_version": return 8;
    case "review_check_update": return { configured: false, channel: "stable", current: "1.2.0", available: false, version: "", notes: "", date: "" };
    case "review_launch_path":
    case "review_take_open_request": return "";
    case "review_open": opened = String(args.path); return { ...accepted(), command: 1 };
    case "review_close": opened = ""; return accepted();
    case "review_set_pixel_to_micron": return null;
    case "fetch_review_info": return info();
    case "fetch_review_rows": {
      const offset = Number(args.offset);
      const total = args.valid ? 250 : 2;
      const n = Math.max(0, Math.min(Number(args.count), total - offset));
      return { valid: true, total: String(total), offset: String(offset), rows: Array.from({ length: n }, (_, k) => row(offset + k)) };
    }
    case "fetch_review_frame":
    case "fetch_review_series_packet":
    case "fetch_review_thumbnails_packet": return new ArrayBuffer(0);
    case "fetch_review_series_count": return "4";
    case "fetch_isoelastic_curves": return { source: "embedded", curves: [] };
    case "fetch_review_scatter":
      return { valid: true, pixel_to_micron: 0.5, frame_index: ["0", "3", "6"], valid_position: ["0", "1", "2"], area_um2: [100, 101, 102], deformability: [0.01, 0.02, 0.03], target_group: [1, 0, 1], ring_ratio: [18, 19, 20] };
    case "review_request_density": return accepted("7");
    case "fetch_review_density": return { valid: true, ready: true, levels: [1, 2, 3], level_count: 8, bandwidth_factor: 1, core_fraction: 0.9, computed_record_json: "" };
    case "review_compute_core": return accepted("8");
    case "review_jobs_busy": return false;
    case "fetch_review_computed_core_json": return COMPUTED_RECORD;
    case "review_save_core_record": return accepted();
    case "review_list_dir": return [];
    case "review_clear_charts": return null;
    case "review_stage_chart": return 1;
    case "review_export_charts": return job(REVIEW_OPERATION_KINDS.ExportCharts, `exported 1 chart(s) to ${args.outputDir}`);
    case "poll_review_events": return { events: queuedEvents.splice(0) };
    case "plugin:dialog|open": return "/data/run.h5";
    case "plugin:dialog|save": return answers.save;
    case "plugin:dialog|message": return "No"; // "Save this core contour?" etc.: decline
    case "plugin:opener|reveal_item_in_dir":
      if (answers.revealFails) throw new Error("no file manager");
      return null;
    default: throw new Error(`unexpected command ${cmd}`);
  }
}

// ---- jsdom stand-ins ---------------------------------------------------------
class NoResizeObserver {
  observe() {}
  unobserve() {}
  disconnect() {}
}
function fakeContext(canvas: HTMLCanvasElement): CanvasRenderingContext2D {
  const gradient = { addColorStop: () => undefined };
  return new Proxy({ canvas } as Record<string | symbol, unknown>, {
    get(target, prop) {
      if (prop in target) return target[prop];
      if (prop === "measureText") return (t: string) => ({ width: String(t).length * 6, actualBoundingBoxAscent: 8, actualBoundingBoxDescent: 2 });
      if (prop === "getImageData" || prop === "createImageData") return () => ({ data: new Uint8ClampedArray(4), width: 1, height: 1 });
      return () => gradient;
    },
    set(target, prop, value) {
      target[prop] = value;
      return true;
    },
  }) as unknown as CanvasRenderingContext2D;
}
class FakePath2D {
  constructor() {
    return new Proxy(this, { get: (t, p) => (p in t ? (t as Record<string | symbol, unknown>)[p] : () => undefined) });
  }
}

let root: Root;
let host: HTMLDivElement;
const logged: string[] = [];
let storage: Map<string, string>;

beforeEach(() => {
  vi.useFakeTimers();
  calls.length = 0;
  opened = "";
  nextOperation = 100;
  queuedEvents.length = 0;
  logged.length = 0;
  answers.save = null;
  answers.revealFails = false;
  storage = new Map();
  vi.stubGlobal("localStorage", {
    getItem: (k: string) => storage.get(k) ?? null,
    setItem: (k: string, v: string) => void storage.set(k, String(v)),
    removeItem: (k: string) => void storage.delete(k),
    clear: () => storage.clear(),
  });
  Object.assign(globalThis, { ResizeObserver: NoResizeObserver, Path2D: FakePath2D });
  HTMLCanvasElement.prototype.getContext = function (this: HTMLCanvasElement) {
    return fakeContext(this);
  } as unknown as typeof HTMLCanvasElement.prototype.getContext;
  HTMLCanvasElement.prototype.toBlob = function (callback: BlobCallback) {
    callback({ arrayBuffer: async () => new Uint8Array([0x89, 0x50, 0x4e, 0x47]).buffer } as unknown as Blob);
  };
  (window as unknown as { __TAURI_INTERNALS__: unknown }).__TAURI_INTERNALS__ = {
    invoke: async (cmd: string, args: Record<string, unknown>, options?: unknown) => {
      if (cmd !== "poll_review_events") calls.push({ cmd, args: JSON.parse(JSON.stringify(args ?? {})), options });
      return respond(cmd, args);
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
  for (let i = 0; i < 4; i++) await act(async () => void (await vi.advanceTimersByTimeAsync(ms)));
}
function button(text: string, scope: ParentNode = host): HTMLButtonElement {
  const found = Array.from(scope.querySelectorAll("button")).filter((b) => b.textContent?.trim() === text);
  if (found.length === 0) throw new Error(`no button "${text}" in: ${Array.from(scope.querySelectorAll("button")).map((b) => b.textContent?.trim()).join(" | ")}`);
  return found[0];
}
async function click(el: Element, ms = 0) {
  await act(async () => void el.dispatchEvent(new MouseEvent("click", { bubbles: true })));
  await settle(ms);
}
const sent = (cmd: string) => calls.filter((c) => c.cmd === cmd);
const selectedIndex = () => host.querySelector("tr.selected td")?.textContent ?? "";
const gridEl = () => host.querySelector(".thumb-grid") as HTMLElement;
const dialog = (label: string) => host.querySelector(`[role="dialog"][aria-label="${label}"]`) as HTMLElement;

async function openPanel() {
  const scheduler = new FramePullScheduler();
  await act(async () => root.render(<ReviewPanel ready={true} scheduler={scheduler} fitWindow={true} log={(l) => void logged.push(l)} fallbackPixelToMicron={0.5} />));
  await settle();
  await click(button("Select HDF File…"));
  await settle();
}
async function openCharts() {
  await click(button("Charts"));
  await settle(400);
}
const pane = () => host.querySelector(".charts-frame") as HTMLElement;
const title = () => host.querySelector(".frame-pane-title")?.textContent ?? "";

async function dispatch(el: Element, type: string, init: MouseEventInit = {}) {
  await act(async () => void el.dispatchEvent(new MouseEvent(type, { bubbles: true, cancelable: true, ...init })));
}

describe("REVIEW-014 thumbnail tooltip", () => {
  it("reads Frame N, a line break and the double-click hint", async () => {
    await openPanel();
    const tiles = Array.from(host.querySelectorAll(".thumb"));
    expect(tiles.length).toBeGreaterThan(1);
    expect(tiles[0].getAttribute("title")).toBe("Frame 0\nDouble-click to view details");
    expect(tiles[1].getAttribute("title")).toBe("Frame 1\nDouble-click to view details");
  });
});

describe("REVIEW-018 Valid / Invalid tabs", () => {
  it("each tab keeps its own selection", async () => {
    await openPanel();
    expect(selectedIndex()).toBe("0");
    await click(host.querySelectorAll("tbody tr")[5]); // valid row 5: frame 15
    expect(selectedIndex()).toBe("15");
    await click(button("Invalid Frames (2)"));
    expect(selectedIndex()).toBe("0"); // first visit: the first item
    await click(host.querySelectorAll("tbody tr")[1]); // invalid row 1: frame 3
    expect(selectedIndex()).toBe("3");
    await click(button("Valid Frames (250)"));
    expect(selectedIndex()).toBe("15");
    await click(button("Invalid Frames (2)"));
    expect(selectedIndex()).toBe("3");
  });

  it("a set that was scrolled is back where it was left", async () => {
    await openPanel();
    const scrollTo = async (top: number) => {
      gridEl().scrollTop = top;
      await act(async () => void gridEl().dispatchEvent(new Event("scroll", { bubbles: true })));
    };
    await scrollTo(300);
    await click(button("Invalid Frames (2)"));
    expect(gridEl().scrollTop).toBe(0); // never scrolled: starts at its selection
    await scrollTo(40);
    await click(button("Valid Frames (250)"));
    expect(gridEl().scrollTop).toBe(300);
    await click(button("Invalid Frames (2)"));
    expect(gridEl().scrollTop).toBe(40);
  });

  it("opening another file starts from the top again", async () => {
    await openPanel();
    gridEl().scrollTop = 500;
    await act(async () => void gridEl().dispatchEvent(new Event("scroll", { bubbles: true })));
    await click(button("Invalid Frames (2)"));
    await click(button("Close File"));
    await settle(5); // the file key carries the open time
    await click(button("Select HDF File…"));
    await click(button("Invalid Frames (2)"));
    await click(button("Valid Frames (250)"));
    expect(gridEl().scrollTop).toBe(0);
    expect(selectedIndex()).toBe("0");
  });
});

describe("REVIEW-028 viewer overlay and ROI", () => {
  it("changing them in the viewer changes the toolbar's, with the viewer still open", async () => {
    await openPanel();
    await act(async () => void host.querySelector(".canvas-wrap.preview")!.dispatchEvent(new MouseEvent("dblclick", { bubbles: true })));
    await settle();
    const viewer = dialog("Frame viewer");
    const combo = viewer.querySelector("select") as HTMLSelectElement;
    await act(async () => {
      combo.value = "3";
      combo.dispatchEvent(new Event("change", { bubbles: true }));
    });
    await click(viewer.querySelector('input[type="checkbox"]')!);
    expect((host.querySelector('select[aria-label="Overlay mode"]') as HTMLSelectElement).value).toBe("3");
    expect((Array.from(host.querySelectorAll(".toolbar label")).find((l) => l.textContent?.trim() === "ROI")!.querySelector("input") as HTMLInputElement).checked).toBe(true);
    expect(dialog("Frame viewer")).not.toBeNull();
    expect(last(sent("fetch_review_frame"))?.args).toMatchObject({ overlay: 3, roiOverlay: true });
  });
});

describe("REVIEW-031 Export as TIFF", () => {
  async function openViewer() {
    await act(async () => void host.querySelector(".canvas-wrap.preview")!.dispatchEvent(new MouseEvent("dblclick", { bubbles: true })));
    await settle();
  }

  it("stages the displayed frame under the chosen name (.tiff appended) and exports it into that folder", async () => {
    await openPanel();
    await openViewer();
    answers.save = "/out/dir/cell one";
    await click(button("Export as TIFF...", dialog("Frame viewer")));
    await settle(250);
    const save = last(sent("plugin:dialog|save"))!;
    expect((save.args.options as { defaultPath: string; title: string }).title).toBe("Export Frame as TIFF");
    expect((save.args.options as { defaultPath: string }).defaultPath).toBe("/data/frame_0_series_0.tiff");
    expect(sent("review_stage_chart").map((c) => (c.options as { headers: Record<string, string> }).headers["x-chart-name"])).toEqual(["cell one.tiff"]);
    expect(sent("review_export_charts").map((c) => c.args)).toEqual([{ outputDir: "/out/dir" }]);
    expect(dialog("Export Frame as TIFF").textContent).toContain("Exported 1 chart(s) to /out/dir");
  });

  it("does nothing when the operator cancels the save dialog", async () => {
    await openPanel();
    await openViewer();
    answers.save = null;
    await click(button("Export as TIFF...", dialog("Frame viewer")));
    expect(sent("review_stage_chart")).toHaveLength(0);
    expect(sent("review_export_charts")).toHaveLength(0);
  });
});

describe("REVIEW-032 Charts frame pane", () => {
  it("shows the Qt empty-state text, then the title for each cell stepped to with Previous / Next", async () => {
    await openPanel();
    await openCharts();
    expect(title()).toBe("Click a point on the scatter to view the cell");
    expect(button("Open in window…", pane()).disabled).toBe(true);
    await click(button("Next ▶", pane()));
    expect(title()).toBe("Frame 0 · valid row 1 of 250");
    await click(button("Next ▶", pane()));
    expect(title()).toBe("Frame 3 · valid row 2 of 250");
    await click(button("Next ▶", pane()));
    expect(title()).toBe("Frame 6 · valid row 3 of 250");
  });

  it("adds the failed-validation note to a cell with no point on the scatter, and Previous wraps", async () => {
    await openPanel();
    await openCharts();
    for (let i = 0; i < 4; i++) await click(button("Next ▶", pane()));
    // Valid-set position 3: the scatter has points for positions 0 – 2 only.
    expect(title()).toBe("Frame 9 · valid row 4 of 250 · failed validation, not on the scatter");
    for (let i = 0; i < 4; i++) await click(button("◀ Previous", pane())); // 2, 1, 0, then round to the last
    expect(title()).toBe("Frame 747 · valid row 250 of 250 · failed validation, not on the scatter");
  });

  it("Open in window… opens the frame viewer on the cell", async () => {
    await openPanel();
    await openCharts();
    await click(button("Next ▶", pane()));
    await click(button("Open in window…", pane()));
    expect(dialog("Frame viewer")).not.toBeNull();
    expect(dialog("Frame viewer").textContent).toContain("valid #1 of 250");
  });

  it("Ctrl+wheel zooms the pane and Fit to Window goes back", async () => {
    await openPanel();
    await openCharts();
    await click(button("Next ▶", pane()));
    const wrap = pane().querySelector(".canvas-wrap") as HTMLElement;
    const canvas = wrap.querySelector("canvas") as HTMLCanvasElement;
    expect(canvas.style.width).toBe("");
    await act(async () => void wrap.dispatchEvent(new WheelEvent("wheel", { bubbles: true, cancelable: true, deltaY: -100, ctrlKey: true })));
    expect(canvas.style.width).not.toBe("");
    await click(button("Fit to Window", pane()));
    expect(canvas.style.width).toBe("");
  });
});

describe("REVIEW-036/037 core share", () => {
  it("the density request and Compute core contour use the Preferences share, 0.9 by default", async () => {
    await openPanel();
    await openCharts();
    expect(last(sent("review_request_density"))?.args).toMatchObject({ coreFraction: 0.9 });
    await click(button("Compute core contour…"), 400);
    expect(last(sent("review_compute_core"))?.args).toEqual({ coreFraction: 0.9 });
  });

  it("follows a stored share, clamped to 0.05 – 1", async () => {
    for (const [stored, expected] of [["0.7", 0.7], ["9", 1], ["0.01", 0.05], ["nonsense", 0.9]] as const) {
      calls.length = 0;
      storage.set(CORE_FRACTION_KEY, stored);
      await openPanel();
      await openCharts();
      await click(button("Compute core contour…"), 400);
      expect(last(sent("review_request_density"))?.args, stored).toMatchObject({ coreFraction: expected });
      expect(last(sent("review_compute_core"))?.args, stored).toEqual({ coreFraction: expected });
      await act(async () => root.unmount());
      root = createRoot(host);
      opened = "";
    }
  });
});

describe("REVIEW-039 Charts splits", () => {
  const handles = () => Array.from(host.querySelectorAll('[role="separator"]')) as HTMLElement[];
  const now = (el: HTMLElement) => el.getAttribute("aria-valuenow");

  it("start at Qt's sizes (600 : 400 and 550 : 300)", async () => {
    await openPanel();
    await openCharts();
    expect(handles().map(now)).toEqual(["60", "65"]);
  });

  it("move with the keyboard and are remembered", async () => {
    await openPanel();
    await openCharts();
    const key = (el: HTMLElement, k: string, shiftKey = false) => act(async () => void el.dispatchEvent(new KeyboardEvent("keydown", { key: k, shiftKey, bubbles: true })));
    await key(handles()[0], "ArrowRight");
    await key(handles()[0], "ArrowLeft", true);
    await key(handles()[1], "ArrowDown");
    expect(handles().map(now)).toEqual(["52", "67"]);
    expect(storage.get(CHARTS_SPLITTER_KEY)).toBe("0.52");
    expect(storage.get(CHARTS_RIGHT_SPLITTER_KEY)).toBe("0.6671"); // 550/850 + 0.02
    // The arrows moved the split, not the selected cell.
    expect(title()).toBe("Click a point on the scatter to view the cell");
    await click(button("Valid Frames (250)"));
    await openCharts();
    expect(handles().map(now)).toEqual(["52", "67"]);
  });

  it("drag with the pointer, stay above the minimum sizes, and save on release", async () => {
    await openPanel();
    await openCharts();
    const box = host.querySelector(".split.row") as HTMLElement;
    box.getBoundingClientRect = () => ({ left: 0, top: 0, width: 1008, height: 600, right: 1008, bottom: 600, x: 0, y: 0, toJSON: () => ({}) });
    const handle = handles()[0];
    await dispatch(handle, "pointerdown", { button: 0, clientX: 600 });
    await dispatch(handle, "pointermove", { clientX: 504 }); // (504 - 4) / 1000 = 0.50
    expect(now(handle)).toBe("50");
    expect(storage.has(CHARTS_SPLITTER_KEY)).toBe(false); // saved once, on release
    await dispatch(handle, "pointermove", { clientX: 100 }); // the scatter keeps its 420 px
    expect(now(handle)).toBe("42");
    await dispatch(handle, "pointermove", { clientX: 990 }); // the pane keeps its 320 px
    expect(now(handle)).toBe("68");
    await dispatch(handle, "pointerup", {});
    expect(storage.get(CHARTS_SPLITTER_KEY)).toBe("0.68");
  });
});

describe("Show in folder failure", () => {
  const completed = { id: "5", kind: REVIEW_OPERATION_KINDS.ExportMetrics, title: "Export Metrics", phase: "completed" as const, progress: 1, total: 1, message: "/data/run_metrics.csv", cancelling: false };

  it("is shown in the dialog and logged, not swallowed", async () => {
    answers.revealFails = true;
    await act(async () => root.render(<JobDialog job={completed} onCancel={() => {}} onClose={() => {}} log={(l) => void logged.push(l)} />));
    expect(host.querySelector('[role="alert"]')).toBeNull();
    await click(button("Show in folder"));
    expect(host.querySelector('[role="alert"]')?.textContent).toContain("Could not open the folder: ");
    expect(host.querySelector('[role="alert"]')?.textContent).toContain("no file manager");
    expect(last(logged)).toContain("show in folder failed: ");
    expect(last(logged)).toContain("no file manager");
  });

  it("clears the message when the next try works", async () => {
    answers.revealFails = true;
    await act(async () => root.render(<JobDialog job={completed} onCancel={() => {}} onClose={() => {}} />));
    await click(button("Show in folder"));
    expect(host.querySelector('[role="alert"]')).not.toBeNull();
    answers.revealFails = false;
    await click(button("Show in folder"));
    expect(host.querySelector('[role="alert"]')).toBeNull();
  });
});

describe("Preferences core share", () => {
  async function openPreferences() {
    await act(async () => root.render(<ReviewApp />));
    await settle();
    await click(button("File"));
    await click(button("Preferences…"));
  }
  const field = () => dialog("Preferences").querySelector("#pref-core") as HTMLInputElement;
  const type = (value: string) =>
    act(async () => {
      const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!;
      setter.call(field(), value);
      field().dispatchEvent(new Event("input", { bubbles: true }));
    });
  const save = () => button("Save", dialog("Preferences"));

  it("shows Qt's 90 % by default and persists a change as a fraction", async () => {
    await openPreferences();
    expect(field().value).toBe("90");
    await type("55");
    expect(save().disabled).toBe(false);
    await click(save());
    expect(storage.get(CORE_FRACTION_KEY)).toBe("0.55");
    await click(button("File"));
    await click(button("Preferences…"));
    expect(field().value).toBe("55");
  });

  it("refuses a share outside 5 – 100 %", async () => {
    await openPreferences();
    for (const bad of ["4", "101", "", "abc"]) {
      await type(bad);
      expect(save().disabled, bad).toBe(true);
    }
    await type("5");
    expect(save().disabled).toBe(false);
    await type("100");
    expect(save().disabled).toBe(false);
    await click(save());
    expect(storage.get(CORE_FRACTION_KEY)).toBe("1");
  });
});
