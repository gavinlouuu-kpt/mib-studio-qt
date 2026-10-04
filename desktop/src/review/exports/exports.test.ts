import { describe, expect, it } from "vitest";
import {
  baseName,
  batchSummaryText,
  defaultMetricsName,
  dirName,
  exportDir,
  joinPath,
  LAST_EXPORT_DIR_KEY,
  nextAvailableName,
  parseBatchSummary,
  parseSeriesRange,
  rememberExportDir,
  seriesRangeFor,
  stem,
} from "./exportHelpers";
import { OPERATION_STATES, REVIEW_OPERATION_KINDS } from "../../bridgeContract";
import { REGENERATE_SOURCE, type ReviewEvent } from "../reviewBridge";
import { regenerateRequest } from "../RegenerateMasks";
import { loadPixelToMicron, PX_TO_UM_KEY } from "../ReviewApp";
import { outcomeText, outputPathOf, shortProgress } from "./ExportDialogs";
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
  const base = { source: REGENERATE_SOURCE.WholeFile as number, sourcePath: "", from: "1", count: "0", outputPath: "/o/x.h5", useRecordedConfig: true, synthesizeBackground: false, setSize: 300 };
  type Form = Parameters<typeof regenerateRequest>[0];
  it("builds requests with 0-based starts", () => {
    expect(regenerateRequest(base as Form)).toEqual({
      ok: true,
      request: { source: REGENERATE_SOURCE.WholeFile, sourcePath: "", startIndex: 0, count: 0, outputPath: "/o/x.h5", useRecordedConfig: true, synthesizeBackground: false },
    });
    const ranged = regenerateRequest({ ...base, source: REGENERATE_SOURCE.CurrentValid, from: "11", count: "20" } as Form);
    expect(ranged.ok && [ranged.request.startIndex, ranged.request.count]).toEqual([10, 20]);
    const avi = regenerateRequest({ ...base, source: REGENERATE_SOURCE.Avi, sourcePath: "/v.avi" } as Form);
    expect(avi.ok && [avi.request.sourcePath, avi.request.useRecordedConfig]).toEqual(["/v.avi", false]);
  });
  it("explains what is missing", () => {
    expect(regenerateRequest({ ...base, outputPath: "" } as Form)).toEqual({ ok: false, error: "Choose where to save the new file" });
    expect(regenerateRequest({ ...base, source: REGENERATE_SOURCE.Folder } as Form)).toEqual({ ok: false, error: "Choose a folder of images" });
    expect(regenerateRequest({ ...base, source: REGENERATE_SOURCE.CurrentValid, from: "0" } as Form).ok).toBe(false);
    expect(regenerateRequest({ ...base, source: REGENERATE_SOURCE.CurrentValid, from: "301" } as Form)).toEqual({ ok: false, error: "This set has 300 images" });
    expect(regenerateRequest({ ...base, source: REGENERATE_SOURCE.CurrentInvalid, count: "-1" } as Form).ok).toBe(false);
  });
});

describe("px→µm preference", () => {
  it("falls back to 1.0 when unset or invalid", () => {
    const s = (v: string | null) => ({ getItem: (k: string) => (k === PX_TO_UM_KEY ? v : null) });
    expect(loadPixelToMicron(s(null))).toBe(1.0);
    expect(loadPixelToMicron(s("0.25"))).toBe(0.25);
    expect(loadPixelToMicron(s("-2"))).toBe(1.0);
    expect(loadPixelToMicron(s("abc"))).toBe(1.0);
    expect(loadPixelToMicron(null)).toBe(1.0);
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
