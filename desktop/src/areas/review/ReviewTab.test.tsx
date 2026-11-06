// @vitest-environment jsdom
// MIB Studio's Review stage against the Qt Review tab (epic #867): the status label and the open-failure
// dialog (REVIEW-003/004), the recording file mode (REVIEW-006), the experiment lock (REVIEW-007) and the
// Area (µm²) column at the file's own factor (REVIEW-068).
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { ReviewMetadata } from "../../bridge";
import { MISSING_FILE_DETAIL } from "../../review/reviewOpen";
import { StudioContext } from "../../studio/StudioContext";
import type { Studio } from "../../studio/useStudioController";
import { REVIEW_LOCKED_EXPERIMENT, REVIEW_LOCKED_RECORDING } from "./reviewLock";

vi.mock("../../transport/index", () => ({ transport: { kind: "tauri", invoke: vi.fn(), session: undefined } }));
vi.mock("../../bridge", async (importOriginal) => ({ ...(await importOriginal<typeof import("../../bridge")>()), bridge: { closeReview: vi.fn() } }));
vi.mock("../../exportControls", () => ({ ReviewExportOptions: () => null }));
vi.mock("../../reanalysisControls", () => ({ ReanalysisControls: () => null }));
vi.mock("../../components/SavedReviewImage", () => ({ SavedReviewImage: () => null }));
vi.mock("../../components/ReviewCharts", () => ({ ReviewCharts: () => null }));
import { ReviewTab } from "./ReviewTab";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let host: HTMLDivElement, root: Root;
beforeEach(() => { host = document.createElement("div"); document.body.append(host); root = createRoot(host); });
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

const dataset = (count: number) => ({ present: count > 0, count, height: 96, width: 512, channels: 1 });
const experimentMeta = (patch: Partial<ReviewMetadata> = {}): ReviewMetadata => ({
  valid: true, file_open: true, recording_file: false, start_time_ns: 0, end_time_ns: 1, total_valid: 250, total_invalid: 2, roi_x: 0, roi_y: 0, roi_w: 512,
  roi_h: 96, has_background: false, has_core_identity: false, core_version: "", core_source: "", core_release_tag: "", valid_images: dataset(250),
  invalid_images: dataset(2), valid_masks: dataset(0), invalid_masks: dataset(0), recorded_images: dataset(0), file_path: "/data/run.h5",
  pixel_to_micron: 0.25, pixel_to_micron_from_file: true, filtered_frames: 0, ...patch,
});
const recordingMeta = (): ReviewMetadata => experimentMeta({
  recording_file: true, total_valid: 1000, total_invalid: 0, valid_images: dataset(0), invalid_images: dataset(0), recorded_images: dataset(1000),
  filtered_frames: 3900, pixel_to_micron: 0.4886, pixel_to_micron_from_file: false, file_path: "/data/rec.h5",
});
const row = (frame_index: number, area: number) => ({
  frame_index, timestamp_ns: 5000 + frame_index, valid: true, target_group: false, object_id: 1, object_count: 1, track_id: 7, centroid_x: 0, centroid_y: 0, area,
  deformability: 0.05, area_ratio: 1, ring_ratio: 18, youngs_modulus: 1.5,
});

function render(patch: Record<string, unknown> = {}) {
  const studio = {
    onSelectHdf: vi.fn(), ready: true, reviewSourceBusy: false, reviewMeta: null, expActive: false, recording: false, reviewing: false, reviewPath: "",
    reviewTab: "raw", reviewOutcome: null, caps: { reanalysis: false }, reviewExport: { busy: false, start: vi.fn() }, stats: null, metricsPage: null,
    metricsOffset: 0, METRICS_PAGE_SIZE: 50, reviewImgIndex: 0, fitWindow: false, reviewCanvasRef: { current: null }, reviewIndex: "0",
    reviewOpenPhase: "idle", reviewOpenFailure: null, setReviewOpenFailure: vi.fn(), ...patch,
  } as unknown as Studio;
  return act(async () => root.render(<StudioContext.Provider value={studio}><ReviewTab /></StudioContext.Provider>));
}
const button = (text: string) => Array.from(host.querySelectorAll("button")).find((b) => b.textContent?.trim() === text);
const status = () => host.querySelector('[data-testid="review-status"]')?.textContent;
const headers = () => Array.from(host.querySelectorAll("th")).map((th) => th.textContent);

describe("status label (REVIEW-004)", () => {
  it("shows Ready, Loading..., Error loading file and the open file's counts", async () => {
    await render();
    expect(status()).toBe("Ready");
    await render({ reviewOpenPhase: "loading" });
    expect(status()).toBe("Loading...");
    await render({ reviewOpenPhase: "error" });
    expect(status()).toBe("Error loading file");
    await render({ reviewMeta: experimentMeta(), reviewing: true, reviewPath: "/data/run.h5" });
    expect(status()).toBe("Valid: 250, Invalid: 2");
  });
});

describe("open-failure dialog (REVIEW-003)", () => {
  it("shows Qt's critical box from the failure and closes it with OK", async () => {
    const setReviewOpenFailure = vi.fn();
    await render({ reviewOpenPhase: "error", reviewOpenFailure: { path: "/data/gone.h5", detail: MISSING_FILE_DETAIL }, setReviewOpenFailure });
    const box = host.querySelector('[role="alertdialog"]')!;
    expect(box.querySelector("h3")!.textContent).toBe("Cannot Open HDF5 File");
    expect(box.querySelector('[data-testid="open-failure-text"]')!.textContent).toBe("Failed to open:\n/data/gone.h5\n\nFile not found.");
    await act(async () => void box.querySelector("button")!.click());
    expect(setReviewOpenFailure).toHaveBeenCalledWith(null);
  });
  it("shows no dialog without a failure", async () => {
    await render();
    expect(host.querySelector('[role="alertdialog"]')).toBeNull();
  });
});

describe("recording file mode (REVIEW-006)", () => {
  const recording = { reviewMeta: recordingMeta(), reviewing: true, reviewPath: "/data/rec.h5", metricsPage: { valid: true, total: 2, offset: 0, rows: [row(0, 0), row(1, 0)] } };
  it("names the raw tab Frames, hides Valid/Invalid, disables charts and metrics export, and says how many frames were skipped", async () => {
    await render(recording);
    expect(button("Frames")).toBeTruthy();
    expect(button("Raw Frames")).toBeUndefined();
    expect(button("Valid Frames")).toBeUndefined();
    expect(button("Invalid Frames")).toBeUndefined();
    expect(button("Charts")!.disabled).toBe(true);
    expect(button("Export Metrics to CSV…")!.disabled).toBe(true);
    expect(button("Export Charts…")!.disabled).toBe(true);
    expect(button("Export All…")!.disabled).toBe(false);
    expect(button("Export Images…")!.disabled).toBe(false);
    expect(status()).toBe("Recording: 1000 frames, 3900 empty skipped");
  });
  it("lists only the index and the timestamp of each frame", async () => {
    await render(recording);
    expect(headers()).toEqual(["Index", "Timestamp"]);
    expect(Array.from(host.querySelectorAll("tbody tr")).map((tr) => tr.textContent)).toEqual(["05000", "15001"]);
  });
  it("keeps Raw/Valid/Invalid for an experiment file, with Export Metrics enabled", async () => {
    await render({ reviewMeta: experimentMeta(), reviewing: true });
    expect(button("Raw Frames")).toBeTruthy();
    expect(button("Valid Frames")).toBeTruthy();
    expect(button("Invalid Frames")).toBeTruthy();
    expect(button("Export Metrics to CSV…")!.disabled).toBe(false);
  });
});

describe("experiment lock (REVIEW-007)", () => {
  it("disables opening and closing a file during an experiment and shows Qt's warning", async () => {
    await render({ expActive: true, reviewMeta: experimentMeta(), reviewing: true });
    expect(button("Select HDF File…")!.disabled).toBe(true);
    expect(button("Select HDF File…")!.title).toBe("Review tab is disabled while an experiment is active. Please stop the experiment first.");
    expect(button("Close File")!.disabled).toBe(true);
    expect(host.querySelector('[data-testid="review-locked"]')!.textContent).toBe(REVIEW_LOCKED_EXPERIMENT);
    expect(host.querySelector("fieldset.review-lock")!.getAttribute("data-review-lock")).toBe("locked");
  });
  it("gives the backend's own reason while raw frames are recorded", async () => {
    await render({ recording: true });
    expect(button("Select HDF File…")!.disabled).toBe(true);
    expect(host.querySelector('[data-testid="review-locked"]')!.textContent).toBe(REVIEW_LOCKED_RECORDING);
  });
  it("leaves the stage open otherwise", async () => {
    await render();
    expect(button("Select HDF File…")!.disabled).toBe(false);
    expect(host.querySelector('[data-testid="review-locked"]')).toBeNull();
    expect(host.querySelector("fieldset.review-lock")!.hasAttribute("disabled")).toBe(false);
  });
});

describe("Area (µm²) column (REVIEW-068)", () => {
  const rows = { valid: true, total: 2, offset: 0, rows: [row(0, 400), row(1, 1000)] };
  it("adds the column after Area (px²), at the file's recorded factor, 2 decimals", async () => {
    await render({ reviewMeta: experimentMeta(), reviewing: true, reviewTab: "valid", metricsPage: rows });
    expect(headers()).toEqual(["Index", "Object Id", "Track Id", "Area (px²)", "Area (µm²)", "Deformability", "Ring ratio", "E (kPa)"]);
    const cells = Array.from(host.querySelectorAll("tbody tr")[0].querySelectorAll("td")).map((td) => td.textContent);
    expect(cells.slice(3, 5)).toEqual(["400.0", "25.00"]); // 400 px² × 0.25² µm²/px²
    expect(Array.from(host.querySelectorAll("tbody tr")[1].querySelectorAll("td"))[4].textContent).toBe("62.50");
  });
  it("uses the live factor the backend reports for a file that records none", async () => {
    await render({ reviewMeta: experimentMeta({ pixel_to_micron: 0.5, pixel_to_micron_from_file: false }), reviewing: true, metricsPage: rows });
    expect(Array.from(host.querySelectorAll("tbody tr")[0].querySelectorAll("td"))[4].textContent).toBe("100.00");
  });
  it("omits the column when an older backend does not report a factor", async () => {
    await render({ reviewMeta: experimentMeta({ pixel_to_micron: undefined }), reviewing: true, metricsPage: rows });
    expect(headers()).not.toContain("Area (µm²)");
    expect(host.querySelectorAll("tbody tr")[0].querySelectorAll("td")).toHaveLength(7);
  });
});
