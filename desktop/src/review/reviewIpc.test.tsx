// @vitest-environment jsdom
// #512 S9: the review module's backend traffic, recorded where it leaves the
// webview (`window.__TAURI_INTERNALS__.invoke`, which every Tauri JS API ends
// in: commands, the dialog plugin and the opener plugin). The flows are the
// operator's: open a file, page the metrics, frames / viewer / series,
// thumbnails, charts and the core contour, every export, regenerate masks,
// Show in folder, close, and the update check. The golden file was recorded
// before the module moved onto `desktop/src/transport/`; the move must leave
// every command, argument, chart byte, header and dialog option unchanged.
// #512 S8 changed one flow on purpose: Regenerate masks lists the output
// directory to propose a free name, and a new flow regenerates into an
// existing file (refused by the backend, the dialog asks again).
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { OPERATION_STATES } from "../bridgeContract";
import { FramePullScheduler } from "../framePullScheduler";
import { REVIEW_OPERATION_KINDS } from "./reviewContract";
import { reviewBridge } from "./reviewBridge";
import { ReviewPanel, type ReviewPanelHandle } from "./ReviewPanel";
import { stageCharts } from "./charts/chartExport";
import { checkUpdate, installUpdate } from "./updates";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

type Recorded = { cmd: string; args: unknown; options?: unknown };
const calls: Recorded[] = [];
let polls = 0;

// Raw bodies (chart PNG bytes) are recorded byte for byte.
function snapshotOf(value: unknown): unknown {
  if (value instanceof Uint8Array) return { rawBytes: Array.from(value) };
  if (value instanceof ArrayBuffer) return { rawBytes: Array.from(new Uint8Array(value)) };
  return value === undefined ? null : JSON.parse(JSON.stringify(value));
}

// ---- a minimal review backend -------------------------------------------------
let opened = "";
let nextOperation = 100;
const queuedEvents: unknown[] = [];
// Files the backend has written (regenerate never replaces one, #512 S8) and
// what the operator answers in the next regenerate save dialogs.
const existingOutputs = new Set<string>();
const regenerateSaveAnswers: string[] = [];

const dataset = (count: number) => ({ present: count > 0, count: String(count), height: 96, width: 512, channels: 1 });
const STORED_RECORD = JSON.stringify({ contours: [[[10, 0.01], [20, 0.02], [30, 0.01]]], core_fraction: 0.8, provisional: false, source: "full_run" });
const COMPUTED_RECORD = JSON.stringify({ contours: [[[11, 0.011], [21, 0.021], [31, 0.011]]], core_fraction: 0.8, provisional: false, source: "full_run", cell_count: 2, population_count: 3 });

function info() {
  const open = opened !== "";
  return {
    valid: true,
    file_open: open,
    file_path: opened,
    recording_file: false,
    start_time_ns: "0",
    end_time_ns: "1000",
    total_valid: open ? "250" : "0",
    total_invalid: open ? "2" : "0",
    filtered_frames: "0",
    roi_x: 0,
    roi_y: 0,
    roi_w: 512,
    roi_h: 96,
    has_background: true,
    has_core_identity: true,
    core_version: "1.2.0",
    core_source: "bundled",
    core_release_tag: "",
    valid_images: dataset(open ? 250 : 0),
    invalid_images: dataset(open ? 2 : 0),
    valid_masks: dataset(open ? 250 : 0),
    invalid_masks: dataset(open ? 2 : 0),
    recorded_images: dataset(0),
    has_series: open,
    series_count: open ? "4" : "0",
    multi_image_enabled: false,
    multi_image_count: "0",
    has_accounting: false,
    accounting_completion: 0,
    accounting_reconciled: false,
    accounting_empty: "0",
    accounting_rejected: "0",
    accounting_processing_failed: "0",
    accounting_store_loss: "0",
    accounting_persisted: "0",
    accounting_admitted: "0",
    accounting_persistence_failed: "0",
    accounting_summary: "",
    pixel_to_micron: 0.5,
    pixel_to_micron_from_file: false,
    kde_analysis_json: open ? STORED_RECORD : "",
    kde_live_json: "",
    has_recorded_config: true,
    ring_ratio_min: 15,
    ring_ratio_max: 25,
  };
}

function row(i: number) {
  return {
    frame_index: String(i * 3),
    timestamp_ns: String(i * 1000),
    valid: true,
    target_group: i % 2 === 0,
    touches_border: false,
    has_single_inner_contour: true,
    in_range: true,
    in_channel: true,
    inner_contour_count: 1,
    object_id: 0,
    object_count: 1,
    track_id: i,
    track_first_frame: String(i * 3),
    track_last_frame: String(i * 3),
    track_observation_count: 1,
    bbox_x: 1,
    bbox_y: 2,
    bbox_width: 30,
    bbox_height: 20,
    centroid_x: 16,
    centroid_y: 12,
    area: 400 + i,
    area_um2: 100 + i,
    deformability: 0.01 * (i + 1),
    area_ratio: 1.01,
    ring_ratio: 18 + (i % 5),
    laplacian_variance: null,
    youngs_modulus: 1.5,
    brightness_q1: 10,
    brightness_q2: 20,
    brightness_q3: 30,
    brightness_q4: 40,
  };
}

const ok = (command = 0) => ({ ok: true, command, message: "accepted", operation_id: "0" });

function job(kind: number, message: string) {
  const id = String(nextOperation++);
  queuedEvents.push({ operation_id: id, kind, state: OPERATION_STATES.Progress, progress: "1", total: "2", message: "working" });
  queuedEvents.push({ operation_id: id, kind, state: OPERATION_STATES.Completed, progress: "2", total: "2", message });
  return { ok: true, command: 0, message: "accepted", operation_id: id };
}

function respond(cmd: string, args: Record<string, unknown>): unknown {
  switch (cmd) {
    case "review_abi_version":
      return 1;
    case "fetch_isoelastic_curves":
      return { source: "embedded", curves: [{ emodulus_kpa: 1, points: [[10, 0.01], [40, 0.03]] }] };
    case "review_launch_path":
    case "review_take_open_request":
      return "";
    case "review_open":
      opened = String(args.path);
      return ok(1);
    case "review_close":
      opened = "";
      return ok(2);
    case "review_set_pixel_to_micron":
      return null;
    case "fetch_review_info":
      return info();
    case "fetch_review_rows": {
      const offset = Number(args.offset);
      const total = args.valid ? 250 : 2;
      const n = Math.max(0, Math.min(Number(args.count), total - offset));
      return { valid: true, total: String(total), offset: String(offset), rows: Array.from({ length: n }, (_, k) => row(offset + k)) };
    }
    case "fetch_review_frame":
    case "fetch_review_series_packet":
    case "fetch_review_thumbnails_packet":
      return new ArrayBuffer(0); // the decoder refuses it: no pixels needed here
    case "fetch_review_series_count":
      return "4";
    case "fetch_review_scatter":
      return {
        valid: true,
        pixel_to_micron: 0.5,
        frame_index: ["0", "3", "6"],
        valid_position: ["0", "1", "2"],
        area_um2: [100, 101, 102],
        deformability: [0.01, 0.02, 0.03],
        target_group: [1, 0, 1],
        ring_ratio: [18, 19, 20],
      };
    case "review_request_density":
      return { ok: true, command: 0, message: "accepted", operation_id: "7" };
    case "fetch_review_density":
      return { valid: true, ready: true, levels: [1, 2, 3], level_count: 8, bandwidth_factor: 1, core_fraction: 0.9, computed_record_json: "" };
    case "review_compute_core":
      return { ok: true, command: 0, message: "accepted", operation_id: "8" };
    case "review_jobs_busy":
      return false;
    case "fetch_review_computed_core_json":
      return COMPUTED_RECORD;
    case "review_save_core_record":
      return ok(3);
    case "review_list_dir":
      return ["run_metrics.csv", ...Array.from(existingOutputs).filter((p) => p.startsWith(`${args.dir}/`)).map((p) => p.slice(String(args.dir).length + 1))];
    case "review_export_metrics":
      return job(REVIEW_OPERATION_KINDS.ExportMetrics, String(args.outputPath));
    case "review_export_all":
      return job(REVIEW_OPERATION_KINDS.ExportAll, `${args.outputRoot}/run`);
    case "review_export_charts":
      return job(REVIEW_OPERATION_KINDS.ExportCharts, `exported 2 chart(s) to ${args.outputDir}`);
    case "review_batch_export":
      return job(REVIEW_OPERATION_KINDS.BatchExport, `exported 2 of 2 file(s)`);
    case "review_regenerate_masks":
      if (existingOutputs.has(String(args.outputPath))) return { ok: false, command: 0, message: "Output already exists; choose a new file", operation_id: "0" };
      existingOutputs.add(String(args.outputPath));
      return job(REVIEW_OPERATION_KINDS.RegenerateMasks, `${args.outputPath} (2 images: 2 valid, 0 invalid)`);
    case "poll_review_events":
      return { events: queuedEvents.splice(0) };
    case "cancel_review_operation":
      return ok(4);
    case "review_clear_charts":
      return null;
    case "review_stage_chart":
      return 1;
    case "review_check_update":
      return { configured: false, channel: String(args.channel), current: "1.2.0", available: false, version: "", notes: "", date: "" };
    case "review_install_update":
      return null;
    // Dialog and opener plugins (what the operator picks).
    case "plugin:dialog|open": {
      const o = args.options as { title?: string; directory?: boolean; multiple?: boolean };
      if (o.title === "AVI source") return "/data/clip.avi";
      if (o.directory) return "/data/out";
      if (o.multiple) return ["/data/a.h5", "/data/b.h5"];
      return "/data/run.h5";
    }
    case "plugin:dialog|save": {
      const o = args.options as { title?: string; defaultPath?: string };
      // Regenerate: the operator takes the proposed name unless told otherwise.
      return o.title === "Save regenerated masks" ? (regenerateSaveAnswers.shift() ?? o.defaultPath) : "/data/out/run_metrics_2.csv";
    }
    case "plugin:dialog|message":
      return "Yes";
    case "plugin:opener|reveal_item_in_dir":
      return null;
    default:
      throw new Error(`unexpected command ${cmd}`);
  }
}

// ---- jsdom stand-ins (canvas, ResizeObserver) ---------------------------------
let pngSerial = 0;
function fakeContext(canvas: HTMLCanvasElement): CanvasRenderingContext2D {
  const state: Record<string | symbol, unknown> = { canvas };
  const gradient = { addColorStop: () => undefined };
  return new Proxy(state, {
    get(target, prop) {
      if (prop in target) return target[prop];
      if (prop === "measureText") return (text: string) => ({ width: String(text).length * 6, actualBoundingBoxAscent: 8, actualBoundingBoxDescent: 2 });
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
    return new Proxy(this, { get: (target, prop) => (prop in target ? (target as Record<string | symbol, unknown>)[prop] : () => undefined) });
  }
}

class NoResizeObserver {
  observe() {}
  unobserve() {}
  disconnect() {}
}

// ---- harness ------------------------------------------------------------------
let root: Root;
let host: HTMLDivElement;
const panel = { current: null as ReviewPanelHandle | null };
const log: string[] = [];

beforeEach(() => {
  vi.useFakeTimers();
  calls.length = 0;
  polls = 0;
  opened = "";
  nextOperation = 100;
  queuedEvents.length = 0;
  existingOutputs.clear();
  regenerateSaveAnswers.length = 0;
  pngSerial = 0;
  log.length = 0;
  const saved = new Map<string, string>();
  vi.stubGlobal("localStorage", {
    getItem: (k: string) => saved.get(k) ?? null,
    setItem: (k: string, v: string) => void saved.set(k, String(v)),
    removeItem: (k: string) => void saved.delete(k),
    clear: () => saved.clear(),
  });
  Object.assign(globalThis, { ResizeObserver: NoResizeObserver, Path2D: FakePath2D });
  HTMLCanvasElement.prototype.getContext = function (this: HTMLCanvasElement) {
    return fakeContext(this);
  } as unknown as typeof HTMLCanvasElement.prototype.getContext;
  HTMLCanvasElement.prototype.toBlob = function (callback: BlobCallback) {
    // Deterministic "PNG" bytes per rendered chart.
    const bytes = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, ++pngSerial, 0xff, 0x00]);
    callback({ arrayBuffer: async () => bytes.buffer.slice(0) } as unknown as Blob);
  };
  (window as unknown as { __TAURI_INTERNALS__: unknown }).__TAURI_INTERNALS__ = {
    invoke: async (cmd: string, args: Record<string, unknown>, options?: unknown) => {
      if (cmd === "poll_review_events") {
        polls++;
        expect(args).toEqual({});
        expect(options).toBeUndefined();
      } else {
        calls.push(options === undefined ? { cmd, args: snapshotOf(args) } : { cmd, args: snapshotOf(args), options: snapshotOf(options) });
      }
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

function modal(label: string): HTMLElement {
  const el = host.querySelector(`[role="dialog"][aria-label="${label}"]`);
  if (!el) throw new Error(`no dialog ${label}`);
  return el as HTMLElement;
}

// Run one flow and return what it sent, in order.
async function flow(name: string, run: () => Promise<void>): Promise<{ flow: string; calls: Recorded[] }> {
  const from = calls.length;
  await run();
  return { flow: name, calls: calls.slice(from) };
}

// The toolbar's More… menu (a toggle that stays open in jsdom: no blur).
async function more(item: string) {
  if (!host.querySelector(".more-menu .menu-popup")) await click(button("More…"));
  await click(button(item, host.querySelector(".more-menu")!));
}

async function finishJob(title: string) {
  await settle(250); // the panel's event drain picks up the terminal event
  const text = modal(title).textContent ?? "";
  if (!text.includes("Written to") && !text.includes("Exported")) throw new Error(`${title} did not complete: ${text} (log: ${log.join(" | ")})`);
  await click(button("Show in folder", modal(title)));
  await click(button("Close", modal(title)));
}

it("sends the same commands, arguments, chart bytes and dialogs as before the transport move", async () => {
  const scheduler = new FramePullScheduler();
  const flows: { flow: string; calls: Recorded[] }[] = [];

  flows.push(
    await flow("mount", async () => {
      await act(async () =>
        root.render(<ReviewPanel ref={(h) => void (panel.current = h)} ready={true} scheduler={scheduler} fitWindow={true} log={(l) => void log.push(l)} fallbackPixelToMicron={0.5} />),
      );
      await settle();
    }),
  );

  flows.push(await flow("open file", () => click(button("Select HDF File…"))));

  flows.push(
    await flow("metrics pages", async () => {
      await click(button("▶"));
      await click(button("◀"));
    }),
  );

  flows.push(
    await flow("invalid set", async () => {
      await click(button("Invalid Frames (2)"));
      await click(button("Valid Frames (250)"));
    }),
  );

  flows.push(
    await flow("frame viewer and series", async () => {
      const preview = host.querySelector(".canvas-wrap.preview")!;
      await act(async () => void preview.dispatchEvent(new MouseEvent("dblclick", { bubbles: true })));
      await settle();
      for (const key of ["ArrowDown", "ArrowDown", "ArrowRight", "Escape"]) {
        await act(async () => void window.dispatchEvent(new KeyboardEvent("keydown", { key })));
        await settle();
      }
    }),
  );

  flows.push(
    await flow("overlay and ROI", async () => {
      const select = host.querySelector('select[aria-label="Overlay mode"]') as HTMLSelectElement;
      await act(async () => {
        select.value = "2";
        select.dispatchEvent(new Event("change", { bubbles: true }));
      });
      await settle();
      const roi = Array.from(host.querySelectorAll("label")).find((l) => l.textContent?.trim() === "ROI")!.querySelector("input")!;
      await click(roi);
    }),
  );

  flows.push(
    await flow("export metrics", async () => {
      await click(button("Export Metrics to CSV…"));
      await finishJob("Export Metrics");
    }),
  );

  flows.push(
    await flow("export all with charts", async () => {
      await click(button("Export All…"));
      await click(button("Export", modal("Export multi-image series")));
      await finishJob("Export All");
    }),
  );

  flows.push(
    await flow("export charts", async () => {
      await more("Export Charts…");
      await finishJob("Export Charts");
    }),
  );

  flows.push(
    await flow("batch metrics", async () => {
      await more("Batch Metrics…");
      await settle(250);
      await click(button("Close", modal("Batch Metrics")));
    }),
  );

  flows.push(
    await flow("batch export all", async () => {
      await more("Batch Export All…");
      await click(button("Export", modal("Export multi-image series")));
      await settle(250);
      await click(button("Close", modal("Batch Export All")));
    }),
  );

  flows.push(
    await flow("charts and core contour", async () => {
      await click(button("Charts"));
      await click(button("Compute core contour…"));
      await settle(400);
    }),
  );

  flows.push(
    await flow("regenerate masks", async () => {
      await more("Regenerate masks…");
      const dialog = modal("Regenerate masks");
      const avi = Array.from(dialog.querySelectorAll("label")).find((l) => l.textContent?.trim() === "AVI file")!.querySelector("input")!;
      await click(avi);
      await click(button("Choose…", dialog));
      await click(button("Save as…", dialog));
      await click(button("Regenerate", dialog));
      await finishJob("Regenerate masks");
    }),
  );

  flows.push(
    await flow("regenerate masks into an existing file", async () => {
      await more("Regenerate masks…");
      const dialog = modal("Regenerate masks");
      const avi = Array.from(dialog.querySelectorAll("label")).find((l) => l.textContent?.trim() === "AVI file")!.querySelector("input")!;
      await click(avi);
      await click(button("Choose…", dialog));
      // The operator picks the file the last run wrote (past the OS's own
      // overwrite question): the backend refuses, nothing is replaced.
      regenerateSaveAnswers.push("/data/out/clip_remasked.h5");
      await click(button("Save as…", dialog));
      await click(button("Regenerate", dialog));
      const refused = modal("Regenerate masks");
      expect(refused.querySelector('[role="alert"]')?.textContent).toBe("Not started: Output already exists; choose a new file (clip_remasked.h5)");
      expect((refused.querySelector('input[aria-label="Output file"]') as HTMLInputElement).value).toBe("");
      expect(host.querySelector('[role="dialog"][aria-label="Regenerate masks"].job-dialog')).toBeNull();
      // Choose again: the proposal is the next free name.
      await click(button("Save as…", refused));
      expect((refused.querySelector('input[aria-label="Output file"]') as HTMLInputElement).value).toBe("/data/out/clip_remasked_2.h5");
      expect(refused.querySelector('[role="alert"]')).toBeNull();
      await click(button("Regenerate", refused));
      await finishJob("Regenerate masks");
    }),
  );

  flows.push(await flow("close file", () => click(button("Close File"))));

  flows.push(
    await flow("bridge calls outside the panel", async () => {
      await reviewBridge.abiVersion();
      await reviewBridge.launchPath();
      await reviewBridge.takeOpenRequest();
      await reviewBridge.cancel("101");
      await reviewBridge.jobsBusy();
      await stageCharts([{ name: "scatter_plot.tiff", bytes: new Uint8Array([1, 2, 3, 250]) }]);
      await stageCharts([]);
      await checkUpdate("stable");
      await installUpdate("beta");
    }),
  );

  // Every flow did reach the backend, and the job dialogs saw their outcomes.
  for (const f of flows) expect(f.calls.length, f.flow).toBeGreaterThan(0);
  expect(polls).toBeGreaterThan(0);
  expect(log.filter((l) => / failed: /.test(l) && !/frame|thumbnail/.test(l))).toEqual([]);

  await expect(JSON.stringify(flows, null, 1) + "\n").toMatchFileSnapshot("./__snapshots__/reviewIpc.golden.json");
});
