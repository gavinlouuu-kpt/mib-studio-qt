// @vitest-environment jsdom
// REVIEW-003 / REVIEW-004 in YOFO Review (epic #867): a file that cannot be opened shows Qt's critical
// "Cannot Open HDF5 File" box and the status "Error loading file", and no longer keeps the previous
// file's info (the backend closes it before it opens the next); a good file shows "Loading..." while it
// opens, then Qt's "Valid: N, Invalid: M" or "Recording: N frames, M empty skipped".
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { FramePullScheduler } from "../framePullScheduler";
import { CORRUPT_FILE_DETAIL } from "./reviewOpen";
import { ReviewPanel, type ReviewPanelHandle } from "./ReviewPanel";
import type { ReviewInfo } from "./reviewBridge";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

const dataset = (count: number) => ({ present: count > 0, count: String(count), height: 96, width: 512, channels: 1 });
function infoFor(path: string): Partial<ReviewInfo> {
  const recording = path.endsWith("rec.h5");
  return {
    valid: true, file_open: true, file_path: path, recording_file: recording, start_time_ns: "0", end_time_ns: "1", total_valid: recording ? "1000" : "250",
    total_invalid: recording ? "0" : "2", filtered_frames: recording ? "3900" : "0", roi_x: 0, roi_y: 0, roi_w: 512, roi_h: 96,
    object_coordinates: "roi_local", object_coordinates_source: "attribute", object_origin_x: 0, object_origin_y: 0, has_background: false,
    has_core_identity: false, core_version: "", core_source: "", core_release_tag: "",
    valid_images: dataset(recording ? 0 : 250), invalid_images: dataset(recording ? 0 : 2), valid_masks: dataset(0), invalid_masks: dataset(0),
    recorded_images: dataset(recording ? 1000 : 0), has_series: false, series_count: "0", multi_image_enabled: false, multi_image_count: "0",
    has_accounting: false, accounting_summary: " · run completed", pixel_to_micron: 0.25, pixel_to_micron_from_file: true,
    kde_analysis_json: "", kde_live_json: "", has_recorded_config: false, ring_ratio_min: 15, ring_ratio_max: 25,
  };
}

let opened = "";
let holdOpen: Promise<void> | null = null;
const MESSAGES: Record<string, string> = {
  "/data/missing.h5": "File not found: /data/missing.h5",
  "/data/bad.h5": "Cannot read the HDF5 file (corrupt or not an HDF5 review file): /data/bad.h5",
};
async function respond(cmd: string, args: Record<string, unknown>): Promise<unknown> {
  switch (cmd) {
    case "review_open": {
      await holdOpen;
      const path = String(args.path);
      if (MESSAGES[path]) { opened = ""; return { ok: false, command: 1, message: MESSAGES[path], operation_id: "0" }; }
      opened = path;
      return { ok: true, command: 1, message: "Review file opened", operation_id: "0" };
    }
    case "review_close": opened = ""; return { ok: true, command: 2, message: "closed", operation_id: "0" };
    case "fetch_review_info": return opened ? infoFor(opened) : { valid: true, file_open: false };
    case "fetch_review_rows": return { valid: true, total: "0", offset: "0", rows: [] };
    case "poll_review_events": return { events: [] };
    default: return new ArrayBuffer(0);
  }
}

let root: Root, host: HTMLDivElement;
const panel = { current: null as ReviewPanelHandle | null };
const infos: (ReviewInfo | null)[] = [];
const files: string[] = [];
const status = () => host.querySelector('[data-testid="review-status"]')!.textContent;
const dialog = () => host.querySelector<HTMLElement>('[role="alertdialog"]');

beforeEach(async () => {
  opened = ""; holdOpen = null; infos.length = 0; files.length = 0;
  Object.assign(globalThis, { ResizeObserver: class { observe() {} unobserve() {} disconnect() {} } });
  HTMLCanvasElement.prototype.getContext = (() => null) as never;
  (window as unknown as { __TAURI_INTERNALS__: unknown }).__TAURI_INTERNALS__ = { invoke: respond, transformCallback: () => 0 };
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
  await act(async () => root.render(
    <ReviewPanel ref={(h) => void (panel.current = h)} ready={false} scheduler={new FramePullScheduler()} fitWindow={true}
      log={() => undefined} onInfo={(i) => void infos.push(i)} onFileChange={(p) => void files.push(p)} />,
  ));
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.restoreAllMocks(); });

const open = (path: string) => act(async () => { await panel.current!.openPath(path); });

it("starts Ready with no file", () => {
  expect(status()).toBe("Ready");
  expect(host.querySelector(".path-label")!.textContent).toBe("No file selected");
});

it("shows Qt's critical dialog for a missing file, the Error loading file status, and no file", async () => {
  await open("/data/missing.h5");
  const box = dialog()!;
  expect(box.querySelector("h3")!.textContent).toBe("Cannot Open HDF5 File");
  expect(box.querySelector('[data-testid="open-failure-text"]')!.textContent).toBe("Failed to open:\n/data/missing.h5\n\nFile not found.");
  expect(status()).toBe("Error loading file");
  expect(infos[infos.length - 1]).toBeNull();
  await act(async () => void box.querySelector("button")!.click());
  expect(dialog()).toBeNull();
  expect(status()).toBe("Error loading file");
});

it("tells a corrupt file apart: the interrupted-write text with the h5recover hint", async () => {
  await open("/data/bad.h5");
  expect(dialog()!.querySelector('[data-testid="open-failure-text"]')!.textContent).toBe(`Failed to open:\n/data/bad.h5\n\n${CORRUPT_FILE_DETAIL}`);
  expect(CORRUPT_FILE_DETAIL).toContain("h5recover");
});

it("drops the previous file's info when the next open fails (the backend closed it)", async () => {
  await open("/data/run.h5");
  expect(host.querySelector(".path-label")!.textContent).toContain("/data/run.h5");
  expect(status()).toBe("Valid: 250, Invalid: 2 · run completed");
  await open("/data/bad.h5");
  expect(host.querySelector(".path-label")!.textContent).toBe("No file selected");
  expect(infos[infos.length - 1]).toBeNull();
  expect(status()).toBe("Error loading file");
  await act(async () => void dialog()!.querySelector("button")!.click());
  await open("/data/run.h5");
  expect(dialog()).toBeNull();
  expect(status()).toBe("Valid: 250, Invalid: 2 · run completed");
});

it("shows Loading... while the file opens, Qt's recording line after, and Ready after Close File", async () => {
  let release!: () => void;
  holdOpen = new Promise<void>((r) => { release = r; });
  let pending!: Promise<void>;
  await act(async () => { pending = panel.current!.openPath("/data/rec.h5"); });
  expect(status()).toBe("Loading...");
  await act(async () => { release(); await pending; });
  expect(status()).toBe("Recording: 1000 frames, 3900 empty skipped · run completed");
  await act(async () => { await panel.current!.closeFile(); });
  expect(status()).toBe("Ready");
});
