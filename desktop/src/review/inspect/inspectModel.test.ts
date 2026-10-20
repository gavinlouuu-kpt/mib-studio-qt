// R3 Inspect's facts (#717 B3a): the record's name, the crop square, which
// image views a record can show, and what the cards say. Nothing is
// invented: a value the file does not record is missing, not a stand-in.
import { describe, expect, it } from "vitest";
import type { ReviewInfo, ReviewRow, ReviewRunIdentity } from "../reviewBridge";
import {
  cropOverlay,
  cropSquare,
  cropUnavailable,
  measurementFacts,
  recordedFacts,
  recordLabel,
  recordProvenance,
  recordWhere,
  storedFacts,
} from "./inspectModel";

const ds = (count: number, present = count > 0) => ({ present, count: String(count), height: 96, width: 512, channels: 1 });
const identity = (over: Partial<ReviewRunIdentity> = {}): ReviewRunIdentity => ({
  has_snapshot: true,
  snapshot_unreadable: false,
  snapshot_version: 3,
  run_id: "run-1",
  experiment_id: "4f1c2a9e-0b3d-4c5e-8f7a-1b2c3d4e5f60",
  run_index: 2,
  sample_ref: "S-02",
  calibration_record_id: "cal-0042",
  operator: null,
  annotation: null,
  annotation_unreadable: false,
  ...over,
});
function info(over: Partial<ReviewInfo> = {}): ReviewInfo {
  return {
    file_open: true,
    file_path: "/data/run.h5",
    recording_file: false,
    valid_images: ds(12),
    invalid_images: ds(3),
    valid_masks: ds(12),
    invalid_masks: ds(3),
    recorded_images: ds(0),
    has_series: false,
    series_count: "0",
    has_core_identity: true,
    core_version: "2.1.0",
    core_source: "bundled",
    pixel_to_micron: 0.25,
    pixel_to_micron_from_file: true,
    accounting_summary: " · run complete — empty 0",
    identity: identity(),
    ...over,
  } as ReviewInfo;
}
function row(over: Partial<ReviewRow> = {}): ReviewRow {
  return {
    frame_index: "82140",
    timestamp_ns: "16428000000",
    valid: true,
    target_group: false,
    object_id: 2,
    object_count: 2,
    track_id: 7,
    track_first_frame: "82100",
    track_last_frame: "82160",
    track_observation_count: 3,
    bbox_x: 100,
    bbox_y: 30,
    bbox_width: 20,
    bbox_height: 30,
    area: 707,
    area_um2: 44.19,
    deformability: 0.0612,
    ring_ratio: 22.05,
    youngs_modulus: 1.42,
    ...over,
  } as ReviewRow;
}
const fact = (facts: { label: unknown; value: unknown; tone?: unknown }[], label: string) => facts.find((f) => f.label === label);

describe("the record's name", () => {
  it("is its frame and object: files carry no record id, none is invented", () => {
    expect(recordLabel(row())).toBe("frame 82140 · object 2");
    expect(recordLabel({ frame_index: "18446744073709551615", object_id: 1 })).toBe("frame 18446744073709551615 · object 1");
    // A raw recording's rows are frames, with no object.
    expect(recordLabel(row({ object_id: 0 }), true)).toBe("frame 82140");
  });

  it("is placed in its set and run, with the run's declared provenance", () => {
    expect(recordWhere(info(), true, 3, 12)).toBe("valid record 4 of 12 · run 2 · sample S-02");
    expect(recordWhere(info(), false, 0, 3)).toBe("invalid record 1 of 3 · run 2 · sample S-02");
    expect(recordWhere(info({ recording_file: true }), true, 0, 5)).toBe("frame 1 of 5 · run 2 · sample S-02");
    expect(recordWhere(info({ identity: identity({ run_index: 0, sample_ref: null }) }), true, 0, 1)).toBe("valid record 1 of 1 · run 0");
    expect(recordProvenance(info())).toBe("experiment 4f1c2a9e… · snapshot v3 · calibration cal-0042");
    expect(recordProvenance(info({ identity: identity({ has_snapshot: false, snapshot_version: null, experiment_id: null, calibration_record_id: null }) }))).toBe(
      "no run snapshot (legacy file)",
    );
    expect(recordProvenance(info({ identity: identity({ has_snapshot: false, snapshot_unreadable: true, experiment_id: null, calibration_record_id: null }) }))).toBe(
      "snapshot unreadable",
    );
  });
});

describe("the crop", () => {
  it("is the square around the bounding box with a margin, inside the image", () => {
    // 30 px tall box × 1.6 = 48, centred on (110, 45).
    expect(cropSquare(row(), 512, 96)).toEqual({ x: 86, y: 21, size: 48 });
    // Near an edge it shifts inside rather than leaving the image.
    expect(cropSquare(row({ bbox_x: 0, bbox_y: 0 }), 512, 96)).toEqual({ x: 0, y: 0, size: 48 });
    expect(cropSquare(row({ bbox_x: 500, bbox_y: 80, bbox_width: 12, bbox_height: 16 }), 512, 96)).toEqual({ x: 486, y: 70, size: 26 });
    // A small box keeps a readable minimum; a huge one never exceeds the image.
    expect(cropSquare(row({ bbox_width: 4, bbox_height: 4 }), 512, 96)?.size).toBe(24);
    expect(cropSquare(row({ bbox_x: 0, bbox_y: 0, bbox_width: 400, bbox_height: 90 }), 512, 96)?.size).toBe(96);
  });

  it("does not exist without a bounding box", () => {
    expect(cropSquare(row({ bbox_width: 0 }), 512, 96)).toBeNull();
    expect(cropSquare(row({ bbox_height: Number.NaN }), 512, 96)).toBeNull();
    expect(cropSquare(row(), 0, 96)).toBeNull();
  });

  it("asks the backend for the overlay of its view", () => {
    expect([cropOverlay("raw"), cropOverlay("contour"), cropOverlay("mask")]).toEqual([0, 1, 3]);
  });

  it("is offered only where the file records what it needs", () => {
    expect(cropUnavailable("full", info({ recording_file: true }), true, null)).toBeNull();
    expect(cropUnavailable("raw", info({ recording_file: true }), true, row())).toBe("Recording files carry no objects");
    expect(cropUnavailable("raw", info(), true, row({ bbox_width: 0 }))).toBe("No bounding box is recorded for this record");
    expect(cropUnavailable("contour", info(), true, null)).toBe("No bounding box is recorded for this record");
    const noMasks = info({ valid_masks: ds(0), invalid_masks: ds(3) });
    expect(cropUnavailable("raw", noMasks, true, row())).toBeNull();
    expect(cropUnavailable("contour", noMasks, true, row())).toBe("No masks are recorded in this set");
    expect(cropUnavailable("mask", noMasks, true, row())).toBe("No masks are recorded in this set");
    expect(cropUnavailable("mask", noMasks, false, row())).toBeNull();
  });
});

describe("the cards' facts", () => {
  it("say what is recorded, from dataset presence", () => {
    const facts = recordedFacts(info({ valid_masks: ds(0), has_series: true, series_count: "3" }), true, row());
    expect(fact(facts, "Measurement")).toMatchObject({ value: "Saved", tone: "confirmed" });
    expect(fact(facts, "Full frame")).toMatchObject({ value: "Saved · 512×96", tone: "confirmed" });
    expect(fact(facts, "Mask / contour")).toMatchObject({ value: "Not recorded", tone: "unknown" });
    expect(fact(facts, "Image series")).toMatchObject({ value: "3 images per record" });
    // The invalid set has no series; a page not read yet has no measurement to claim.
    const invalid = recordedFacts(info({ has_series: true, series_count: "3" }), false, null);
    expect(fact(invalid, "Image series")?.value).toBe("None recorded");
    expect(fact(invalid, "Measurement")?.value).toBe("Not recorded");
    const recording = recordedFacts(info({ recording_file: true, recorded_images: ds(5) }), true, null);
    expect(recording.map((f) => f.label)).toEqual(["Frame image", "Measurement", "Mask / contour"]);
    expect(fact(recording, "Measurement")?.value).toBe("Not recorded (raw recording)");
  });

  it("give the stored values as stored", () => {
    const facts = measurementFacts(row(), info());
    expect(fact(facts, "Area")?.value).toBe("44.19 µm² · 707.0 px²");
    expect(fact(facts, "E (stored)")?.value).toBe("1.42 kPa");
    expect(fact(facts, "Track")?.value).toBe("track 7 · frames 82100–82160 · 3 observations");
    expect(fact(facts, "Analysis")?.value).toBe("stored · core 2.1.0 (bundled)");
    expect(fact(facts, "Timestamp")?.value).toBe("16428000000 ns");
    // Out of the LUT's range: missing, not 0. Untracked: said so.
    expect(fact(measurementFacts(row({ youngs_modulus: Number.NaN, track_id: -1 }), info()), "E (stored)")?.value).toBeNull();
    expect(fact(measurementFacts(row({ track_id: -1 }), info()), "Track")?.value).toBe("Not tracked");
    expect(fact(measurementFacts(row(), info({ has_core_identity: false })), "Analysis")?.value).toBe("stored · core not recorded");
  });

  it("give a raw recording's frame and time only, never its unfilled values", () => {
    // The backend fills a recording's row with the frame index and timestamp
    // only; the rest stays at its defaults (0, invalid, untracked).
    const blank = row({ valid: false, object_id: 0, object_count: 0, track_id: -1, area: 0, area_um2: 0, deformability: 0, ring_ratio: 0, youngs_modulus: 0 });
    const facts = measurementFacts(blank, info({ recording_file: true }));
    expect(facts.map((f) => f.label)).toEqual([
      "Frame",
      "Timestamp",
      "Area",
      "Deformability",
      "E (stored)",
      "Ring ratio",
      "Validation",
      "Track",
      "Objects in frame",
      "Analysis",
    ]);
    expect(fact(facts, "Frame")?.value).toBe("82140");
    expect(fact(facts, "Timestamp")?.value).toBe("16428000000 ns");
    for (const f of facts.slice(2, 9)) expect(f, String(f.label)).toMatchObject({ value: "Not recorded", tone: "unknown" });
    expect(fact(facts, "Analysis")).toMatchObject({ value: "None (raw recording)", tone: "unknown" });
    for (const f of facts) expect(String(f.value), String(f.label)).not.toMatch(/^0(\.0*)?\b|\binvalid\b|µm²|kPa/);
  });

  it("say what produced the stored results, and how reprocessing works", () => {
    const facts = storedFacts(info());
    // Neutral: a regenerated file records the core that regenerated it, and
    // no marker tells it from a capture, so "at acquisition" would be a claim.
    expect(fact(facts, "Stored results")?.value).toBe("core 2.1.0 (bundled) recorded in this file");
    expect(fact(facts, "Run snapshot")?.value).toBe("v3 · run run-1");
    expect(fact(facts, "Calibration used")?.value).toBe("cal-0042 · px→µm 0.2500 (recorded)");
    expect(fact(facts, "E-LUT")).toMatchObject({ value: "Not available", tone: "unknown" });
    expect(fact(facts, "Run accounting")?.value).toBe("run complete — empty 0");
    expect(fact(facts, "Reprocessing")?.value).toBe("Reprocessing writes a new file; this view lists no history");
    const legacy = storedFacts(
      info({ has_core_identity: false, pixel_to_micron: 0.4886, pixel_to_micron_from_file: false, accounting_summary: "", identity: identity({ has_snapshot: false, snapshot_version: null, calibration_record_id: null }) }),
    );
    expect(fact(legacy, "Stored results")?.value).toBeNull();
    expect(fact(legacy, "Run snapshot")?.value).toBeNull();
    expect(fact(legacy, "Calibration used")).toMatchObject({ value: "px→µm 0.4886 (fallback: the file records none)", tone: "warn" });
    expect(fact(legacy, "Run accounting")?.value).toBeNull();
  });
});
