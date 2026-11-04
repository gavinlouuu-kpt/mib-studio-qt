import { describe, expect, it } from "vitest";
import {
  baseName,
  batchSummaryText,
  defaultFcsName,
  defaultMetricsName,
  defaultRegenerateName,
  eventMapPathOf,
  dirName,
  exportDir,
  joinPath,
  LAST_EXPORT_DIR_KEY,
  MAX_NAME_PROBES,
  MAX_NAME_SUFFIX,
  nextAvailableName,
  parseBatchSummary,
  parseSeriesRange,
  rememberExportDir,
  seriesRangeFor,
  stem,
  withFcsExtension,
} from "./exportHelpers";
import { OPERATION_STATES } from "../../bridgeContract";
import { REVIEW_OPERATION_KINDS } from "../reviewContract";
import { REGENERATE_SOURCE, type ReviewEvent } from "../reviewBridge";
import { regenerateCalibration, regenerateRequest } from "../RegenerateMasks";
import { DEFAULT_REGEN_CONFIG } from "../regenerate/regenerateForm";
import { loadPixelToMicron, PX_TO_UM_KEY } from "../ReviewApp";
import { fcsOutcomeText, outcomeText, outputPathOf, parseFcsOutcome, shortProgress } from "./ExportDialogs";
import { applyJobEvent, isTerminal, type TrackedJob } from "./useReviewJobs";

describe("paths", () => {
  it("splits POSIX and Windows paths", () => {
    expect(baseName("/data/run.h5")).toBe("run.h5");
    expect(baseName("C:\\data\\run.h5")).toBe("run.h5");
    expect(dirName("/data/run.h5")).toBe("/data");
    expect(dirName("C:\\data\\run.h5")).toBe("C:\\data");
    expect(dirName("/run.h5")).toBe("/");
    expect(dirName("run.h5")).toBe("");
    expect(stem("/d/run.v2.h5")).toBe("run.v2");
    expect(joinPath("/d", "x.csv")).toBe("/d/x.csv");
    expect(joinPath("C:\\d", "x.csv")).toBe("C:\\d\\x.csv");
    expect(joinPath("/d/", "x.csv")).toBe("/d/x.csv");
  });
});

describe("default export names (HdfExportService::nextAvailableName)", () => {
  it("uses <basename>_metrics.csv when free", () => {
    expect(defaultMetricsName("/d/run.h5", [])).toBe("run_metrics.csv");
    expect(defaultMetricsName("/d/run.h5", ["other.csv"])).toBe("run_metrics.csv");
  });
  it("suffixes _2, _3 when taken", () => {
    expect(defaultMetricsName("/d/run.h5", ["run_metrics.csv"])).toBe("run_metrics_2.csv");
    expect(defaultMetricsName("/d/run.h5", ["run_metrics.csv", "run_metrics_2.csv"])).toBe("run_metrics_3.csv");
  });
  it("goes past the highest suffix in use, like the backend", () => {
    expect(defaultMetricsName("/d/run.h5", ["run_metrics.csv", "run_metrics_7.csv"])).toBe("run_metrics_8.csv");
    expect(nextAvailableName(["run", "run_x"], "run", "run_", "")).toBe("run_2");
  });
  it("proposes a free regenerate output: regenerate never replaces a file (#512 S8)", () => {
    expect(defaultRegenerateName("run", [])).toBe("run_remasked.h5");
    expect(defaultRegenerateName("run", ["run_metrics.csv", "other_remasked.h5"])).toBe("run_remasked.h5");
    expect(defaultRegenerateName("run", ["run_remasked.h5"])).toBe("run_remasked_2.h5");
    expect(defaultRegenerateName("run", ["run_remasked.h5", "run_remasked_2.h5"])).toBe("run_remasked_3.h5");
    expect(defaultRegenerateName("run", ["run_remasked.h5", "run_remasked_4.h5"])).toBe("run_remasked_5.h5");
  });
  it("treats names as taken ignoring case: Windows and macOS volumes are case-insensitive (#735)", () => {
    expect(defaultRegenerateName("run", ["RUN_remasked.h5"])).toBe("run_remasked_2.h5");
    expect(defaultRegenerateName("run", ["Run_Remasked.H5", "run_REMASKED_2.h5"])).toBe("run_remasked_3.h5");
    expect(defaultRegenerateName("run", ["run_remasked.h5", "RUN_REMASKED_7.H5"])).toBe("run_remasked_8.h5");
    expect(defaultMetricsName("/d/run.h5", ["RUN_METRICS.CSV"])).toBe("run_metrics_2.csv");
  });
  it("ignores suffixes too large to step, bounds the search, falls back to a timestamp (#735)", () => {
    // 2^53: one past Number.MAX_SAFE_INTEGER, where c + 1 === c and the old loop never ended.
    expect(defaultRegenerateName("run", ["run_remasked.h5", "run_remasked_9007199254740992.h5"])).toBe("run_remasked_2.h5");
    expect(defaultRegenerateName("run", ["run_remasked.h5", "run_remasked_1000000000.h5"])).toBe("run_remasked_2.h5");
    const now = new Date(2026, 9, 19, 10, 35, 12);
    expect(defaultRegenerateName("run", ["run_remasked.h5", `run_remasked_${MAX_NAME_SUFFIX}.h5`], now)).toBe("run_remasked_20261019-103512.h5");
    expect(
      defaultRegenerateName("run", ["run_remasked.h5", `run_remasked_${MAX_NAME_SUFFIX}.h5`, "RUN_remasked_20261019-103512.h5"], now),
    ).toBe("run_remasked_20261019-103512-2.h5");
    // The bound holds even when the stamped names are all taken.
    const stamped = Array.from({ length: MAX_NAME_PROBES }, (_, k) => (k < 2 ? "run_remasked_20261019-103512.h5" : `run_remasked_20261019-103512-${k}.h5`));
    expect(nextAvailableName(["run_remasked.h5", `run_remasked_${MAX_NAME_SUFFIX}.h5`, ...stamped], "run_remasked.h5", "run_remasked_", ".h5", now)).toMatch(
      /^run_remasked_20261019-103512-\d+\.h5$/,
    );
  });
});

describe("series range prompt", () => {
  it("parses 1-based ranges into 0-based inclusive", () => {
    expect(parseSeriesRange("9-15")).toEqual({ ok: true, choice: { kind: "range", start: 8, end: 14 } });
    expect(parseSeriesRange(" 3 – 4 ")).toEqual({ ok: true, choice: { kind: "range", start: 2, end: 3 } });
    expect(parseSeriesRange("5")).toEqual({ ok: true, choice: { kind: "range", start: 4, end: 4 } });
  });
  it("clamps to the record size and rejects nonsense", () => {
    expect(parseSeriesRange("9-15", 12)).toEqual({ ok: true, choice: { kind: "range", start: 8, end: 11 } });
    expect(parseSeriesRange("13-15", 12).ok).toBe(false);
    expect(parseSeriesRange("0-3").ok).toBe(false);
    expect(parseSeriesRange("5-2").ok).toBe(false);
    expect(parseSeriesRange("a-b").ok).toBe(false);
    expect(parseSeriesRange("").ok).toBe(false);
  });
  it("maps choices onto the bridge range", () => {
    expect(seriesRangeFor({ kind: "all" })).toEqual({ exportSeries: true, start: 0, end: "" });
    expect(seriesRangeFor({ kind: "skip" })).toEqual({ exportSeries: false, start: 0, end: "" });
    expect(seriesRangeFor({ kind: "range", start: 8, end: 14 })).toEqual({ exportSeries: true, start: 8, end: 14 });
  });
});

describe("remembered export directory", () => {
  it("prefers the last successful directory, else the file's", () => {
    const store = new Map<string, string>();
    const s = { getItem: (k: string) => store.get(k) ?? null, setItem: (k: string, v: string) => void store.set(k, v) };
    expect(exportDir("/data/run.h5", s)).toBe("/data");
    rememberExportDir("/exports", s);
    expect(store.get(LAST_EXPORT_DIR_KEY)).toBe("/exports");
    expect(exportDir("/data/run.h5", s)).toBe("/exports");
    rememberExportDir("", s);
    expect(exportDir("/data/run.h5", s)).toBe("/exports");
    expect(exportDir("/data/run.h5", null)).toBe("/data");
  });
});

describe("batch summary", () => {
  it("parses the job's terminal message", () => {
    expect(parseBatchSummary("exported 3 of 3 file(s)")).toEqual({ exported: 3, total: 3, failures: [] });
    const s = parseBatchSummary("exported 1 of 3 file(s); failed: missing.h5: cannot open missing.h5; bad.h5: destination already exists: /x");
    expect(s).toEqual({
      exported: 1,
      total: 3,
      failures: [
        { file: "missing.h5", reason: "cannot open missing.h5" },
        { file: "bad.h5", reason: "destination already exists: /x" },
      ],
    });
    expect(batchSummaryText(s!)).toBe("Exported 1 of 3 file(s); 2 failed.");
    expect(batchSummaryText({ exported: 2, total: 2, failures: [] })).toBe("Exported all 2 file(s).");
    expect(parseBatchSummary("something else")).toBeNull();
  });
});

const job = (over: Partial<TrackedJob> = {}): TrackedJob => ({
  id: "7",
  kind: REVIEW_OPERATION_KINDS.ExportAll,
  title: "Export All",
  phase: "running",
  progress: 0,
  total: 0,
  message: "",
  cancelling: false,
  ...over,
});
const ev = (state: number, over: Partial<ReviewEvent> = {}): ReviewEvent => ({ operation_id: "7", kind: REVIEW_OPERATION_KINDS.ExportAll, state, progress: "0", total: "0", message: "", ...over });

describe("job tracking", () => {
  it("folds progress and exactly one terminal state", () => {
    let j = applyJobEvent(job(), ev(OPERATION_STATES.Progress, { progress: "3", total: "10", message: "ValidImages: a.tiff" }));
    expect([j.progress, j.total, j.message]).toEqual([3, 10, "ValidImages: a.tiff"]);
    j = applyJobEvent(j, ev(OPERATION_STATES.Completed, { message: "/out/run" }));
    expect(j.phase).toBe("completed");
    expect(applyJobEvent(j, ev(OPERATION_STATES.Failed, { message: "late" }))).toBe(j); // terminal is final
    expect(applyJobEvent(job(), ev(OPERATION_STATES.Progress, { operation_id: "8" }))).toEqual(job()); // other job
    expect(applyJobEvent(job(), ev(OPERATION_STATES.Cancelled, { message: "partial discarded" })).phase).toBe("cancelled");
    expect(applyJobEvent(job(), ev(OPERATION_STATES.TimedOut)).phase).toBe("failed");
    expect([OPERATION_STATES.Started, OPERATION_STATES.Progress].some(isTerminal)).toBe(false);
  });

  it("describes outcomes and finds the output path", () => {
    expect(outcomeText(job({ phase: "completed", message: "/out/run" }))).toBe("Written to /out/run");
    expect(outputPathOf(job({ phase: "completed", message: "/out/run" }))).toBe("/out/run");
    const regen = job({ kind: REVIEW_OPERATION_KINDS.RegenerateMasks, phase: "completed", message: "/o/x.h5 (300 images: 3 valid, 297 invalid)" });
    expect(outputPathOf(regen)).toBe("/o/x.h5");
    // #758: the message also names the factor the objects were validated at.
    const withFactor = job({ kind: REVIEW_OPERATION_KINDS.RegenerateMasks, phase: "completed", message: "/o/x.h5 (4 images: 4 valid, 0 invalid; px→µm 0.2500 fallback: the file records none)" });
    expect(outputPathOf(withFactor)).toBe("/o/x.h5");
    // #764: and the E-LUT E was computed with.
    const withLut = job({ kind: REVIEW_OPERATION_KINDS.RegenerateMasks, phase: "completed", message: "/o/x.h5 (4 images: 4 valid, 0 invalid; px→µm 0.2500 recorded; E-LUT lut-a r1 (sha256 0123456789ab) (the source run's))" });
    expect(outputPathOf(withLut)).toBe("/o/x.h5");
    const batch = job({ kind: REVIEW_OPERATION_KINDS.BatchExport, phase: "completed", message: "exported 1 of 2 file(s); failed: b.h5: cannot open" });
    expect(outcomeText(batch)).toBe("Exported 1 of 2 file(s); 1 failed.");
    expect(outputPathOf(batch)).toBe("");
    expect(outcomeText(job({ phase: "cancelled" }))).toBe("Cancelled. Partial output discarded.");
    expect(outcomeText(job({ phase: "cancelled", message: "partial output was discarded" }))).toBe("Cancelled. Partial output was discarded");
    expect(outcomeText(job({ id: "0", phase: "failed", message: "Another review job is still running" }))).toBe("Not started: Another review job is still running");
    expect(outcomeText(job({ phase: "failed", message: "disk full" }))).toBe("Failed: disk full");
  });
});

describe("regenerate masks form", () => {
  const base = {
    source: REGENERATE_SOURCE.WholeFile as number,
    sourcePath: "",
    from: "1",
    count: "100",
    outputPath: "/o/x.h5",
    setSize: 300,
    config: DEFAULT_REGEN_CONFIG,
    roi: { x: 2, y: 2, w: 28, h: 20 },
    background: { kind: "recorded" } as const,
    synthesizeBackground: true,
  };
  type Form = Parameters<typeof regenerateRequest>[0];
  it("builds review_regenerate_masks_json requests with 0-based starts (#867)", () => {
    expect(regenerateRequest(base as Form)).toEqual({
      ok: true,
      request: {
        source: REGENERATE_SOURCE.WholeFile,
        source_path: "",
        start: 0,
        count: 0,
        output_path: "/o/x.h5",
        use_recorded_config: true,
        synthesize_background: true,
        image_processing: DEFAULT_REGEN_CONFIG,
        roi: { x: 2, y: 2, w: 28, h: 20 },
        background: "recorded",
      },
    });
    const ranged = regenerateRequest({ ...base, source: REGENERATE_SOURCE.CurrentValid, from: "11", count: "20" } as Form);
    expect(ranged.ok && [ranged.request.start, ranged.request.count]).toEqual([10, 20]);
    const avi = regenerateRequest({ ...base, source: REGENERATE_SOURCE.Avi, sourcePath: "/v.avi", roi: null } as Form);
    expect(avi.ok && [avi.request.source_path, avi.request.use_recorded_config, avi.request.roi]).toEqual(["/v.avi", false, { x: 0, y: 0, w: 0, h: 0 }]);
    // Set as Background: the source index and, for the open file, its dataset.
    const frame = regenerateRequest({ ...base, background: { kind: "frame", index: 41, dataset: "/valid_frames/images", label: 2 } } as Form);
    expect(frame.ok && [frame.request.background, frame.request.background_index, frame.request.background_dataset]).toEqual(["frame", 41, "/valid_frames/images"]);
    const aviFrame = regenerateRequest({ ...base, source: REGENERATE_SOURCE.Avi, sourcePath: "/v.avi", background: { kind: "frame", index: 3, dataset: "", label: 4 } } as Form);
    expect(aviFrame.ok && aviFrame.request.background_dataset).toBeUndefined();
    // Only the eight Processing Config fields are sent: every other setting stays the base config's.
    const edited = regenerateRequest({ ...base, config: { ...DEFAULT_REGEN_CONFIG, gaussian_blur_size: 7 } } as Form);
    expect(edited.ok && Object.keys(edited.request.image_processing!).sort()).toEqual(
      ["area_threshold_max", "area_threshold_min", "bg_subtract_threshold", "deformability_threshold_max", "deformability_threshold_min", "gaussian_blur_size", "morph_iterations", "morph_kernel_size"],
    );
    expect(edited.ok && edited.request.image_processing!.gaussian_blur_size).toBe(7);
  });
  it("explains what is missing", () => {
    expect(regenerateRequest({ ...base, outputPath: "" } as Form)).toEqual({ ok: false, error: "Choose where to save the new file" });
    expect(regenerateRequest({ ...base, source: REGENERATE_SOURCE.Folder } as Form)).toEqual({ ok: false, error: "Choose a folder of images" });
    expect(regenerateRequest({ ...base, source: REGENERATE_SOURCE.CurrentValid, from: "0" } as Form).ok).toBe(false);
    expect(regenerateRequest({ ...base, source: REGENERATE_SOURCE.CurrentValid, from: "301" } as Form)).toEqual({ ok: false, error: "This set has 300 images" });
    expect(regenerateRequest({ ...base, source: REGENERATE_SOURCE.CurrentInvalid, count: "-1" } as Form).ok).toBe(false);
    expect(regenerateRequest({ ...base, roi: { x: 20, y: 0, w: 20, h: 10 }, frameWidth: 32, frameHeight: 24 } as Form)).toEqual({
      ok: false,
      error: "The ROI reaches past the 32 × 24 frame",
    });
  });
  it("names the factor and E-LUT the run will use (REVIEW-064)", () => {
    const info = (over: object) => ({ file_open: true, pixel_to_micron: 0.5, pixel_to_micron_from_file: true, identity: { emodulus_lut: null, emodulus_lut_provenance: null }, ...over }) as never;
    expect(regenerateCalibration(info({}), false)).toEqual([
      "Validated at 0.5000 µm/px (recorded in the file)",
      "E from the review app's own E-LUT, if one is loaded: the source records none",
    ]);
    expect(regenerateCalibration(info({ pixel_to_micron: 0.4886, pixel_to_micron_from_file: false }), false)[0]).toBe(
      "Validated at 0.4886 µm/px (fallback from Preferences: the file records none)",
    );
    const lut = { loaded: true, lut_id: "lut-a", revision: "r1" };
    expect(regenerateCalibration(info({ identity: { emodulus_lut: lut, emodulus_lut_provenance: "snapshot" } }), false)[1]).toBe("E from the source run's E-LUT lut-a r1");
    expect(regenerateCalibration(info({ identity: { emodulus_lut: { ...lut, loaded: false }, emodulus_lut_provenance: "snapshot" } }), false)[1]).toBe(
      "E not computed: the source run loaded no E-LUT",
    );
    expect(regenerateCalibration(info({}), true)[0]).toBe("Validated at 0.4886 µm/px (the default: AVI and folder sources record no factor)");
  });
});

describe("px→µm preference", () => {
  it("falls back to the Qt tab's 0.4886 when unset or invalid", () => {
    const s = (v: string | null) => ({ getItem: (k: string) => (k === PX_TO_UM_KEY ? v : null) });
    expect(loadPixelToMicron(s(null))).toBe(0.4886);
    expect(loadPixelToMicron(s("0.25"))).toBe(0.25);
    expect(loadPixelToMicron(s("1"))).toBe(1);
    expect(loadPixelToMicron(s("-2"))).toBe(0.4886);
    expect(loadPixelToMicron(s("abc"))).toBe(0.4886);
    expect(loadPixelToMicron(null)).toBe(0.4886);
  });
});

describe("progress text", () => {
  it("shortens paths to file names", () => {
    expect(shortProgress("valid_images: /tmp/x/.run.partial-1/valid_frame_005141.tiff")).toBe("valid_images: valid_frame_005141.tiff");
    expect(shortProgress("Charts: C:\\out\\.p\\scatter_plot.tiff")).toBe("Charts: scatter_plot.tiff");
    expect(shortProgress("Metrics")).toBe("Metrics");
  });
});

describe("chart export outcome", () => {
  it("names the folder and capitalises", () => {
    const j = job({ kind: REVIEW_OPERATION_KINDS.ExportCharts, phase: "completed", message: "exported 2 chart(s) to /out/charts" });
    expect(outputPathOf(j)).toBe("/out/charts");
    expect(outcomeText(j)).toBe("Exported 2 chart(s) to /out/charts");
  });
});

describe("FCS export (#576, #867)", () => {
  it("proposes the Qt default: <base>.fcs, else the first free <base>_N pair", () => {
    expect(defaultFcsName("/d/run.h5", [])).toBe("run.fcs");
    expect(defaultFcsName("/d/run.h5", ["run.fcs"])).toBe("run_2.fcs");
    // Either half of a pair takes the name, ignoring case.
    expect(defaultFcsName("/d/run.h5", ["RUN_event_map.csv", "run_2.fcs"])).toBe("run_3.fcs");
    // The first gap, not one past the highest (Qt uniqueFcsPath).
    expect(defaultFcsName("/d/run.h5", ["run.fcs", "run_3.fcs", "run_3_event_map.csv"])).toBe("run_2.fcs");
  });
  it("appends .fcs and pairs the event map", () => {
    expect(withFcsExtension("/o/cells")).toBe("/o/cells.fcs");
    expect(withFcsExtension("/o/cells.FCS")).toBe("/o/cells.FCS");
    expect(withFcsExtension("/o/cells.csv")).toBe("/o/cells.csv.fcs");
    expect(eventMapPathOf("/o/cells.fcs")).toBe("/o/cells_event_map.csv");
    expect(eventMapPathOf("C:\\o\\cells.fcs")).toBe("C:\\o\\cells_event_map.csv");
  });
  it("reads the job's message as the Qt success text", () => {
    const message = "/o/run.fcs (3 events: 2 valid, 1 invalid)";
    expect(parseFcsOutcome(message)).toEqual({ path: "/o/run.fcs", events: 3, valid: 2, invalid: 1 });
    expect(parseFcsOutcome("/o/run.fcs")).toEqual({ path: "/o/run.fcs" });
    expect(fcsOutcomeText(message)).toBe("Exported 3 FCS 3.1 events (2 valid, 1 invalid) to /o/run.fcs. Event map: /o/run_event_map.csv");
    const j = job({ kind: REVIEW_OPERATION_KINDS.ExportFcs, phase: "completed", message });
    expect(outputPathOf(j)).toBe("/o/run.fcs");
    expect(outcomeText(j)).toBe(fcsOutcomeText(message));
  });
  it("keeps MIB Studio's remembered folder apart from YOFO Review's", () => {
    const saved = new Map<string, string>();
    const storage = { getItem: (k: string) => saved.get(k) ?? null, setItem: (k: string, v: string) => void saved.set(k, v) };
    rememberExportDir("/studio", storage, "mib.review.lastExportDir");
    expect(exportDir("/data/run.h5", storage)).toBe("/data");
    expect(exportDir("/data/run.h5", storage, "mib.review.lastExportDir")).toBe("/studio");
  });
});

describe("updates", () => {
  it("defaults to the stable channel", async () => {
    const { loadUpdateChannel, updateSummary, UPDATE_CHANNEL_KEY } = await import("../updates");
    const s = (v: string | null) => ({ getItem: (k: string) => (k === UPDATE_CHANNEL_KEY ? v : null) });
    expect(loadUpdateChannel(s(null))).toBe("stable");
    expect(loadUpdateChannel(s("beta"))).toBe("beta");
    expect(loadUpdateChannel(s("nightly"))).toBe("stable");
    expect(loadUpdateChannel(null)).toBe("stable");
    const base = { configured: true, channel: "stable", current: "1.2.0", available: false, version: "", notes: "", date: "" };
    expect(updateSummary({ ...base, configured: false })).toMatch(/no update key/);
    expect(updateSummary(base)).toBe("YOFO Review 1.2.0 is the latest version on the stable channel.");
    expect(updateSummary({ ...base, available: true, version: "1.3.0" })).toBe("YOFO Review 1.3.0 is available (you have 1.2.0).");
  });
});
