// R1 Open's pure model (#717 B1): folder listings, recorded times, the run
// accounting chip and the open run's facts. Absent stays absent.
import { describe, expect, it } from "vitest";
import { RUN_COMPLETION_STATES } from "../../bridgeContract";
import type { ReviewInfo } from "../reviewBridge";
import { completionChip, openRunFacts, recordedTime, runFilesIn } from "./openModel";

const dataset = (count: number) => ({ present: count > 0, count: String(count), height: 96, width: 512, channels: 1 });

function info(over: Partial<ReviewInfo> = {}): ReviewInfo {
  return {
    valid: true,
    file_open: true,
    file_path: "/data/lib/run_02.h5",
    recording_file: false,
    start_time_ns: "1760848200000000000",
    end_time_ns: "0",
    total_valid: "250",
    total_invalid: "2",
    filtered_frames: "0",
    roi_x: 0,
    roi_y: 0,
    roi_w: 512,
    roi_h: 96,
    has_background: true,
    has_core_identity: true,
    core_version: "2.1.0",
    core_source: "bundled",
    core_release_tag: "",
    valid_images: dataset(250),
    invalid_images: dataset(2),
    valid_masks: dataset(250),
    invalid_masks: dataset(2),
    recorded_images: dataset(0),
    has_series: false,
    series_count: "0",
    multi_image_enabled: false,
    multi_image_count: "0",
    has_accounting: true,
    accounting_completion: RUN_COMPLETION_STATES.Complete,
    accounting_reconciled: true,
    accounting_empty: "0",
    accounting_rejected: "0",
    accounting_processing_failed: "0",
    accounting_store_loss: "0",
    accounting_persisted: "252",
    accounting_admitted: "252",
    accounting_persistence_failed: "0",
    accounting_summary: " · run complete — empty 0, rejected 0",
    pixel_to_micron: 0.25,
    pixel_to_micron_from_file: true,
    kde_analysis_json: "",
    kde_live_json: "",
    has_recorded_config: true,
    ring_ratio_min: 15,
    ring_ratio_max: 25,
    // Review ABI 2 (#746): a legacy file, every identity member absent.
    object_coordinates: "unknown",
    object_coordinates_source: "",
    object_origin_x: 0,
    object_origin_y: 0,
    identity: {
      has_snapshot: false,
      snapshot_unreadable: false,
      snapshot_version: null,
      run_id: null,
      experiment_id: null,
      run_index: null,
      sample_ref: null,
      calibration_record_id: null,
      operator: null,
      annotation: null,
      annotation_unreadable: false,
      emodulus_lut: null,
      profile_id: null,
    },
    ...over,
  };
}

const fact = (facts: { label: unknown; value: unknown; tone?: string }[], label: string) => facts.find((f) => f.label === label);

describe("runFilesIn", () => {
  it("keeps .h5 / .hdf5 files in natural order and drops hidden work files", () => {
    const runs = runFilesIn("/data/lib", ["run_10.h5", "notes.txt", "run_2.H5", ".run_3.h5.partial-1", "b.hdf5", "run_1.h5", "metrics.csv"]);
    expect(runs.map((r) => r.name)).toEqual(["b.hdf5", "run_1.h5", "run_2.H5", "run_10.h5"]);
    expect(runs[1].path).toBe("/data/lib/run_1.h5");
    expect(runFilesIn("C:\\runs", ["a.h5"])[0].path).toBe("C:\\runs\\a.h5");
    expect(runFilesIn("/x", [])).toEqual([]);
  });
});

describe("recordedTime", () => {
  it("is null when the file records no time", () => {
    for (const absent of ["0", "000", "", null, undefined, "not a number", "-5"]) expect(recordedTime(absent)).toBeNull();
    expect(recordedTime("1760848200000000000")).toMatch(/^2025-10-1\d \d\d:\d\d$/);
  });
});

describe("completionChip", () => {
  it("never reads missing accounting as complete", () => {
    expect(completionChip(false, RUN_COMPLETION_STATES.Complete)).toEqual({ tone: "unknown", label: "Accounting · not recorded" });
    expect(completionChip(true, null).tone).toBe("unknown");
    expect(completionChip(true, RUN_COMPLETION_STATES.Complete)).toEqual({ tone: "confirmed", label: "Complete" });
    expect(completionChip(true, RUN_COMPLETION_STATES.IncompleteLoss)).toEqual({ tone: "warn", label: "Partial · loss" });
    expect(completionChip(true, RUN_COMPLETION_STATES.Failed).tone).toBe("blocked");
    expect(completionChip(true, RUN_COMPLETION_STATES.Unknown).tone).toBe("unknown");
  });
});

describe("openRunFacts", () => {
  it("states what the file records and leaves the rest absent", () => {
    const facts = openRunFacts(info());
    expect(fact(facts, "File")?.value).toBe("run_02.h5");
    expect(fact(facts, "Folder")?.value).toBe("/data/lib");
    expect(fact(facts, "Records")?.value).toBe("250 valid · 2 invalid");
    expect(fact(facts, "Ended")?.value).toBeNull();
    expect(fact(facts, "Pixel → µm")?.tone).toBeUndefined();
    expect(fact(facts, "Run accounting")?.value).toBe("run complete — empty 0, rejected 0");
  });

  it("marks a legacy file's missing accounting, factor and core", () => {
    const facts = openRunFacts(info({ has_accounting: false, accounting_summary: " · accounting: not recorded (legacy file)", pixel_to_micron_from_file: false, has_core_identity: false, start_time_ns: "0" }));
    expect(fact(facts, "Run accounting")).toMatchObject({ value: "Not recorded · legacy file", tone: "unknown" });
    expect(fact(facts, "Pixel → µm")).toMatchObject({ tone: "warn" });
    expect(String(fact(facts, "Pixel → µm")?.value)).toContain("fallback");
    expect(fact(facts, "Processing core")?.value).toBeNull();
    expect(fact(facts, "Started")?.value).toBeNull();
  });

  it("describes a raw recording by its frames", () => {
    expect(fact(openRunFacts(info({ recording_file: true, total_valid: "9" })), "Records")?.value).toBe("9 frames");
  });
});
