// R3 Inspect's facts (#717 B3a, B3c): the record's name, the crop square
// placed through the file's object coordinate space, which image views a
// record can show, what the cards say (E-LUT, profile, space), the linked
// records and the record reprocessed. Nothing is invented: a value the file
// does not record is missing or "Not recorded", not a stand-in.
import { describe, expect, it } from "vitest";
import type { ReviewEModulusLut, ReviewInfo, ReviewReprocess, ReviewRow, ReviewRunIdentity, ReviewTrackRows } from "../reviewBridge";
import {
  CROP_UNKNOWN_SPACE,
  cropOverlay,
  cropSquare,
  cropUnavailable,
  lutFact,
  measurementFacts,
  objectOffset,
  objectSpaceFact,
  placedBox,
  profileFact,
  recordCrop,
  recordedFacts,
  recordLabel,
  recordProvenance,
  recordWhere,
  DIFFERENT_LUT,
  reprocessComparison,
  reprocessLut,
  reprocessLutDiffers,
  reprocessMatchText,
  reprocessUnavailable,
  storedEText,
  storedENotComputed,
  storedFacts,
  trackSummary,
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
  emodulus_lut: null,
  profile_id: null,
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
    roi_x: 0,
    roi_y: 0,
    roi_w: 512,
    roi_h: 96,
    object_coordinates: "image",
    object_coordinates_source: "roi_origin",
    object_origin_x: 0,
    object_origin_y: 0,
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
    // A v3 snapshot without a profile or an E-LUT says so; it never fills one in.
    expect(recordProvenance(info())).toBe("experiment 4f1c2a9e… · snapshot v3 · calibration cal-0042 · profile not recorded · E-LUT not recorded");
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

  it("is placed through the file's object coordinate space (ruling on #717, 2026-10-21)", () => {
    // A capture with its ROI at (40, 24) stores ROI-local boxes next to
    // full-frame images: the crop is cut at box + origin.
    const roiLocal = info({ roi_x: 40, roi_y: 24, roi_w: 200, roi_h: 60, object_coordinates: "roi_local", object_coordinates_source: "attribute", object_origin_x: 40, object_origin_y: 24 });
    expect(objectOffset(roiLocal)).toEqual({ dx: 40, dy: 24 });
    expect(placedBox(row(), roiLocal)).toEqual({ bbox_x: 140, bbox_y: 54, bbox_width: 20, bbox_height: 30 });
    // 30 px tall box × 1.6 = 48, centred on (150, 69) → (126, 45).
    expect(recordCrop(row(), roiLocal, 512, 96)).toEqual({ x: 126, y: 45, size: 48 });
    expect(cropUnavailable("raw", roiLocal, true, row())).toBeNull();
    // The same ROI moved to (0, 8): the crop moves with it.
    const moved = info({ ...roiLocal, roi_x: 0, roi_y: 8, object_origin_x: 0, object_origin_y: 8 });
    expect(recordCrop(row(), moved, 512, 96)).toEqual({ x: 86, y: 29, size: 48 });
    // A regenerated file stores image coordinates: as stored, whatever its ROI says.
    const image = info({ roi_x: 40, roi_y: 24, roi_w: 200, roi_h: 60, object_coordinates: "image", object_coordinates_source: "attribute", object_origin_x: 0, object_origin_y: 0 });
    expect(objectOffset(image)).toEqual({ dx: 0, dy: 0 });
    expect(recordCrop(row(), image, 512, 96)).toEqual(cropSquare(row(), 512, 96));
    // An image file is never shifted even if an origin were sent with it.
    expect(objectOffset(info({ object_coordinates: "image", object_origin_x: 40, object_origin_y: 24 }))).toEqual({ dx: 0, dy: 0 });
  });

  it("is not cut when nothing decides the space, nor for a box outside the image", () => {
    const unknown = info({ roi_x: 4, roi_y: 2, roi_w: 200, roi_h: 60, object_coordinates: "unknown", object_coordinates_source: "", object_origin_x: 0, object_origin_y: 0 });
    expect(objectOffset(unknown)).toBeNull();
    expect(placedBox(row(), unknown)).toBeNull();
    expect(recordCrop(row(), unknown, 512, 96)).toBeNull();
    for (const view of ["raw", "contour", "mask"] as const) expect(cropUnavailable(view, unknown, true, row()), view).toBe(CROP_UNKNOWN_SPACE);
    expect(CROP_UNKNOWN_SPACE).toBe(
      "No cell crop: the file does not record whether its bounding boxes are relative to its ROI or to the image, and its masks do not tell",
    );
    expect(cropUnavailable("full", unknown, true, row())).toBeNull();
    // A recording never has objects, whatever its space reads.
    expect(objectOffset(info({ recording_file: true, object_coordinates: "image" }))).toBeNull();
    expect(cropUnavailable("raw", info({ recording_file: true, object_coordinates: "unknown" }), true, row())).toBe("Recording files carry no objects");
    expect(objectOffset(null)).toBeNull();
    // Bounds: a placed box wholly outside the image is not cut at an edge.
    const far = info({ object_coordinates: "roi_local", object_coordinates_source: "snapshot", object_origin_x: 600, object_origin_y: 0 });
    expect(recordCrop(row(), far, 512, 96)).toBeNull();
    expect(cropSquare(row({ bbox_x: -40, bbox_width: 20 }), 512, 96)).toBeNull();
    // Partly outside: kept inside the image, as before.
    expect(cropSquare(row({ bbox_x: 505, bbox_width: 20 }), 512, 96)).toEqual({ x: 464, y: 21, size: 48 });
  });

  it("says what decided the file's space", () => {
    const space = (over: Partial<ReviewInfo>) => objectSpaceFact(info(over));
    expect(space({ object_coordinates: "roi_local", object_coordinates_source: "attribute", object_origin_x: 40, object_origin_y: 24 })).toEqual({
      label: "Object coordinates",
      value: "ROI-local, placed at the ROI origin (40, 24) · recorded in the file (@object_coordinates)",
    });
    expect(space({ object_coordinates: "roi_local", object_coordinates_source: "snapshot", object_origin_x: 40, object_origin_y: 24 }).value).toContain("decided by the run snapshot");
    expect(space({ object_coordinates: "image", object_coordinates_source: "mask_check" }).value).toBe("image · decided by checking the stored masks");
    expect(space({ object_coordinates: "image", object_coordinates_source: "roi_origin" }).value).toBe(
      "image · the ROI starts at (0, 0) or has no area, so both placements are the same",
    );
    expect(space({ object_coordinates: "unknown", object_coordinates_source: "" })).toMatchObject({ tone: "warn" });
    expect(String(space({ object_coordinates: "unknown", object_coordinates_source: "" }).value)).toMatch(/^Unknown: the file does not record it/);
    expect(space({ recording_file: true }).value).toBe("None (raw recording)");
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
    // A v3 snapshot that records neither: "Not recorded", never a value.
    expect(fact(facts, "E-LUT")).toMatchObject({ value: "Not recorded", tone: "unknown" });
    expect(fact(facts, "Profile")).toMatchObject({ value: "Not recorded", tone: "unknown" });
    expect(fact(facts, "Object coordinates")?.value).toBe("image · the ROI starts at (0, 0) or has no area, so both placements are the same");
    expect(facts.map((f) => f.label)).toEqual(["Stored results", "Run snapshot", "Calibration used", "Profile", "E-LUT", "Object coordinates", "Run accounting", "Reprocessing"]);
    expect(fact(facts, "Run accounting")?.value).toBe("run complete — empty 0");
    expect(fact(facts, "Reprocessing")?.value).toBe("Reprocessing writes a new file; this view lists no history");
    const legacy = storedFacts(
      info({ has_core_identity: false, pixel_to_micron: 0.4886, pixel_to_micron_from_file: false, accounting_summary: "", identity: identity({ has_snapshot: false, snapshot_version: null, calibration_record_id: null }) }),
    );
    expect(fact(legacy, "Stored results")?.value).toBeNull();
    expect(fact(legacy, "Run snapshot")?.value).toBeNull();
    expect(fact(legacy, "Calibration used")).toMatchObject({ value: "px→µm 0.4886 (fallback: the file records none)", tone: "warn" });
    expect(fact(legacy, "Run accounting")?.value).toBeNull();
    expect(fact(legacy, "E-LUT")).toMatchObject({ value: "Not recorded", tone: "unknown" });
    expect(fact(legacy, "Profile")).toMatchObject({ value: "Not recorded", tone: "unknown" });
  });
});

const LUT: ReviewEModulusLut = {
  loaded: true,
  lut_id: "emod-lut-2026a",
  revision: "3",
  source: "catalog",
  sha256: "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08",
  catalog_sha256: "aa",
  checksum_status: "verified",
};

describe("the E-LUT and profile (review ABI 3)", () => {
  it("give the recorded provenance, as recorded", () => {
    const id = identity({ emodulus_lut: LUT, profile_id: "rbc-default" });
    expect(lutFact(id)).toEqual({ label: "E-LUT", value: "emod-lut-2026a · revision 3 · catalog · sha256 9f86d081884c… · checksum verified", tone: "confirmed" });
    expect(profileFact(id)).toEqual({ label: "Profile", value: "rbc-default" });
    expect(recordProvenance(info({ identity: id }))).toBe("experiment 4f1c2a9e… · snapshot v3 · calibration cal-0042 · profile rbc-default · E-LUT emod-lut-2026a rev 3");
    expect(fact(storedFacts(info({ identity: id })), "E-LUT")?.value).toContain("emod-lut-2026a");
    expect(fact(storedFacts(info({ identity: id })), "Profile")?.value).toBe("rbc-default");
  });

  it("leave out members not recorded, and say when no LUT was loaded", () => {
    const partial = identity({ emodulus_lut: { ...LUT, revision: null, source: null, sha256: null, checksum_status: "unknown" } });
    expect(lutFact(partial)).toEqual({ label: "E-LUT", value: "emod-lut-2026a · checksum unknown", tone: undefined });
    const noId = identity({ emodulus_lut: { ...LUT, lut_id: null, revision: null } });
    expect(lutFact(noId).value).toMatch(/^id not recorded · catalog/);
    const none = identity({ emodulus_lut: { ...LUT, loaded: false, lut_id: null, revision: null, source: null, sha256: null, catalog_sha256: null, checksum_status: null } });
    expect(lutFact(none)).toEqual({ label: "E-LUT", value: "none loaded (the run had no E values)", tone: "warn" });
    expect(recordProvenance(info({ identity: none }))).toContain("no E-LUT loaded");
    // A legacy file: the provenance line says it has no snapshot, nothing about members.
    const legacy = identity({ has_snapshot: false, snapshot_version: null, experiment_id: null, calibration_record_id: null });
    expect(recordProvenance(info({ identity: legacy }))).toBe("no run snapshot (legacy file)");
    expect(lutFact(undefined)).toMatchObject({ value: "Not recorded" });
    expect(profileFact(null)).toMatchObject({ value: "Not recorded" });
  });
});

describe("the linked records", () => {
  const t = (over: Partial<ReviewTrackRows>): ReviewTrackRows => ({ ok: true, message: "", total: "3", truncated: false, positions: ["0", "4", "9"], rows: [row(), row(), row()], ...over });
  it("say how many records the track has, and when the cap cut them", () => {
    expect(trackSummary(t({}), 7, true)).toBe("Track 7: 3 records in the valid set");
    expect(trackSummary(t({ total: "1", positions: ["2"], rows: [row()] }), 7, false)).toBe("Track 7: 1 record in the invalid set");
    const capped = t({ total: "300", truncated: true, positions: Array.from({ length: 256 }, (_, k) => String(k)), rows: Array.from({ length: 256 }, () => row()) });
    expect(trackSummary(capped, 7, true)).toBe("Track 7: 300 records in the valid set; the first 256 are shown (at most 256 per request)");
  });
});

describe("the record reprocessed", () => {
  const stored = row({ area: 707, area_um2: 44.19, deformability: 0.0612, area_ratio: 1.02, ring_ratio: 22.05, youngs_modulus: 1.42, object_count: 1, centroid_x: 10, centroid_y: 15, bbox_x: 0, bbox_y: 0 });
  const found = row({ area: 712, area_um2: 44.5, deformability: 0.0601, area_ratio: 1.03, ring_ratio: 22.1, youngs_modulus: Number.NaN, bbox_x: 40, bbox_y: 24, centroid_x: 50, centroid_y: 39 });
  const result = (over: Partial<ReviewReprocess>): ReviewReprocess => ({
    ok: true,
    message: "",
    settings: 0,
    used_recorded_settings: true,
    settings_label: "recorded settings (config, ROI, background); px→µm 0.2500 recorded; E not recomputed (no E-LUT loaded)",
    core_version: "2.1.0",
    core_source: "bundled",
    pixel_to_micron: 0.25,
    emodulus_recomputed: false,
    emodulus_lut: null,
    emodulus_lut_sha256: null,
    stored,
    reprocessed: [found],
    match_index: 0,
    object_count_differs: false,
    truncated: false,
    ...over,
  });
  const roiLocal = info({ object_coordinates: "roi_local", object_coordinates_source: "attribute", object_origin_x: 40, object_origin_y: 24 });

  it("puts the stored values next to the matched object's, in image coordinates", () => {
    const lines = reprocessComparison(result({}), roiLocal);
    const line = (label: string) => lines.find((l) => l.label === label);
    expect(line("Area (px²)")).toEqual({ label: "Area (px²)", stored: "707.0", reprocessed: "712.0" });
    expect(line("Deformability")).toMatchObject({ stored: "0.0612", reprocessed: "0.0601" });
    // The stored ROI-local box is placed at + (40, 24), as the backend matched it.
    expect(line("Box (image x, y · w×h)")).toMatchObject({ stored: "40, 24 · 20×30", reprocessed: "40, 24 · 20×30" });
    expect(line("Centroid (image)")).toMatchObject({ stored: "50.0, 39.0", reprocessed: "50.0, 39.0" });
    // E without a LUT: said so, never 0 or NaN.
    expect(line("E")).toMatchObject({ stored: "1.42 kPa", reprocessed: "not recomputed (no E-LUT loaded)" });
    expect(line("Objects in frame")).toMatchObject({ stored: "1", reprocessed: "1" });
    expect(reprocessMatchText(result({}))).toBe("Matched object 1 of 1 object found (centroid nearest the stored one, inside the stored box)");
    // Image-space files are shown as stored.
    expect(reprocessComparison(result({}), info()).find((l) => l.label === "Box (image x, y · w×h)")?.stored).toBe("0, 0 · 20×30");
    // A backend before review ABI 4 names no LUT (the fields are absent): a
    // recomputed E keeps the B3c caveat (the app's current one, not
    // necessarily the run's).
    const recomputed = result({ emodulus_recomputed: true, emodulus_lut: undefined, emodulus_lut_sha256: undefined, reprocessed: [{ ...found, youngs_modulus: 1.5 }] });
    expect(reprocessComparison(recomputed, info()).find((l) => l.label === "E")?.reprocessed).toBe(
      "1.50 kPa, computed with the app's current E-LUT, which may differ from the run's",
    );
    expect(reprocessMatchText(recomputed)).toBe(
      "Matched object 1 of 1 object found (centroid nearest the stored one, inside the stored box); its E was computed with the app's current E-LUT, which may differ from the run's",
    );
    // Not recomputed: no E to compare, so no caveat on the match line.
    expect(reprocessMatchText(result({}))).not.toContain("E-LUT");
  });

  describe("names the E-LUT the reprocess used (B3d, review ABI 4)", () => {
    const SHA = LUT.sha256!;
    const OTHER = "8a86b2beef4f0123456789abcdef0123456789abcdef0123456789abcdef0123";
    const named = (over: Partial<ReviewReprocess> = {}) => result({
      emodulus_recomputed: true, emodulus_lut: "emod-lut-2026a 3 (sha256 9f86d081884c)", emodulus_lut_sha256: SHA,
      settings_label: "recorded settings; E-LUT emod-lut-2026a 3 (sha256 9f86d081884c) (the run's)",
      reprocessed: [{ ...found, youngs_modulus: 1.5 }], ...over,
    });
    const runLut = info({ identity: identity({ emodulus_lut: LUT }) });
    const eLine = (r: ReviewReprocess, i: ReviewInfo) => reprocessComparison(r, i).find((l) => l.label === "E");

    it("the run's LUT, matched by sha256: named, no caveat", () => {
      expect(reprocessLut(named(), runLut)).toEqual({ kind: "run", words: "E-LUT emod-lut-2026a 3 (sha256 9f86d081884c)", run: "" });
      expect(eLine(named(), runLut)).toEqual({ label: "E", stored: "1.42 kPa", reprocessed: "1.50 kPa, E-LUT emod-lut-2026a 3 (sha256 9f86d081884c), the run's" });
      expect(reprocessMatchText(named(), runLut)).toBe(
        "Matched object 1 of 1 object found (centroid nearest the stored one, inside the stored box); its E was computed with the run's E-LUT emod-lut-2026a 3 (sha256 9f86d081884c)",
      );
      expect(reprocessMatchText(named(), runLut)).not.toContain("may differ");
      // The hash compares case-insensitively.
      expect(reprocessLut(named({ emodulus_lut_sha256: SHA.toUpperCase() }), runLut).kind).toBe("run");
      expect(reprocessLutDiffers(named(), runLut)).toBe(false);
    });

    it("the run records no LUT hash: named, nothing compared; a name without the hash gets the short form", () => {
      const noHash = info({ identity: identity({ emodulus_lut: { ...LUT, sha256: null } }) });
      expect(reprocessLut(named(), noHash).kind).toBe("named");
      expect(eLine(named(), info())?.reprocessed).toBe("1.50 kPa, E-LUT emod-lut-2026a 3 (sha256 9f86d081884c)");
      expect(reprocessLut(named({ emodulus_lut: "emod-lut-2026a 3" }), info()).words).toBe("E-LUT emod-lut-2026a 3 (sha256 9f86d081884c)");
      expect(reprocessMatchText(named(), info())).toContain("; its E was computed with E-LUT emod-lut-2026a 3 (sha256 9f86d081884c)");
    });

    it("a different LUT than the run's: the E row is marked not comparable, and the match line says so", () => {
      const other = named({ emodulus_lut: "scaled_isoelastic_data_LUT_6.16-4.24 bundled (sha256 8a86b2beef4f)", emodulus_lut_sha256: OTHER });
      expect(reprocessLut(other, runLut)).toMatchObject({ kind: "different", run: "sha256 9f86d081884c" });
      const e = eLine(other, runLut)!;
      expect(e.warn).toBe(true);
      expect(e.reprocessed).toBe(`1.50 kPa, E-LUT scaled_isoelastic_data_LUT_6.16-4.24 bundled (sha256 8a86b2beef4f) · ${DIFFERENT_LUT} (the run's: sha256 9f86d081884c)`);
      expect(DIFFERENT_LUT).toBe("different E-LUT: not comparable");
      expect(reprocessMatchText(other, runLut)).toBe(
        "Matched object 1 of 1 object found (centroid nearest the stored one, inside the stored box); its E was computed with E-LUT scaled_isoelastic_data_LUT_6.16-4.24 bundled (sha256 8a86b2beef4f), not the run's (sha256 9f86d081884c): E is not comparable",
      );
      expect(reprocessLutDiffers(other, runLut)).toBe(true);
      // A run that loaded no LUT, reprocessed with one, is not comparable either.
      const noneLoaded = info({ identity: identity({ emodulus_lut: { ...LUT, loaded: false, lut_id: null, revision: null, sha256: null } }) });
      expect(reprocessLut(other, noneLoaded)).toMatchObject({ kind: "different", run: "none loaded" });
      // No match: no E row to mark.
      expect(eLine(named({ match_index: null }), runLut)?.warn).toBeUndefined();
      // A matched object without an E value: the match line claims no computed E.
      const noValue = named({ emodulus_lut: other.emodulus_lut, emodulus_lut_sha256: OTHER, reprocessed: [{ ...found, youngs_modulus: Number.NaN }] });
      expect(reprocessMatchText(noValue, runLut)).toBe("Matched object 1 of 1 object found (centroid nearest the stored one, inside the stored box)");
      expect(reprocessLutDiffers(noValue, runLut)).toBe(false);
      expect(eLine(noValue, runLut)).toMatchObject({ reprocessed: null });
    });

    it("not recomputed: says so, never names a LUT", () => {
      const not = result({ emodulus_recomputed: false, emodulus_lut: null, emodulus_lut_sha256: null });
      expect(reprocessLut(not, runLut).kind).toBe("not_recomputed");
      expect(eLine(not, runLut)?.reprocessed).toBe("not recomputed (no E-LUT loaded)");
      expect(reprocessMatchText(not, runLut)).not.toContain("E-LUT");
    });

    it("an older backend (fields absent) keeps the B3c caveat, only then", () => {
      const older = named({ emodulus_lut: undefined, emodulus_lut_sha256: undefined });
      expect(reprocessLut(older, runLut).kind).toBe("unnamed");
      expect(eLine(older, runLut)?.reprocessed).toBe("1.50 kPa, computed with the app's current E-LUT, which may differ from the run's");
      expect(reprocessMatchText(named(), runLut)).not.toContain("which may differ");
    });
  });

  it("says when nothing matches, and leaves the reprocessed side empty", () => {
    const none = result({ match_index: null, reprocessed: [found, found] });
    expect(reprocessComparison(none, roiLocal).filter((l) => l.label !== "Objects in frame").every((l) => l.reprocessed === null)).toBe(true);
    expect(reprocessComparison(none, roiLocal).find((l) => l.label === "Objects in frame")?.reprocessed).toBe("2");
    expect(reprocessMatchText(none)).toBe("No match: the kernel found 2 objects, none with its centroid inside the stored box");
    expect(reprocessMatchText(result({ match_index: null, reprocessed: [] }))).toBe("No match: the kernel found no object in the record's stored image");
    const many = result({ match_index: 3, truncated: true, reprocessed: Array.from({ length: 64 }, () => found) });
    // Truncated: the backend matched among the 64 it returned only.
    expect(reprocessMatchText(many)).toBe("Matched object 4 of the first 64 objects returned (the kernel found more; a nearer one may not be returned)");
    expect(reprocessMatchText({ ...many, match_index: null })).toBe(
      "No match among the first 64 objects returned (the kernel found more); one not returned may lie inside the stored box",
    );
    expect(reprocessComparison(many, roiLocal).find((l) => l.label === "Objects in frame")?.reprocessed).toBe("more than 64");
    // A stored count of 0 is not a count.
    expect(reprocessComparison(result({ stored: { ...stored, object_count: 0 } }), roiLocal).find((l) => l.label === "Objects in frame")?.stored).toBeNull();
    expect(reprocessComparison(result({ ok: false, stored: null }), roiLocal)).toEqual([]);
  });

  it("is offered only where the backend would take it", () => {
    expect(reprocessUnavailable(info(), true, false, false)).toBeNull();
    expect(reprocessUnavailable(roiLocal, true, false, false)).toBeNull();
    expect(reprocessUnavailable(info(), false, false, false)).toBe("Select a record to reprocess");
    expect(reprocessUnavailable(null, true, false, false)).toBe("Select a record to reprocess");
    expect(reprocessUnavailable(info({ recording_file: true }), true, false, false)).toBe("Recording files carry no objects");
    expect(reprocessUnavailable(info({ object_coordinates: "unknown", object_coordinates_source: "" }), true, false, false)).toMatch(
      /^Not offered: the file does not record whether its bounding boxes are relative to its ROI or to the image/,
    );
    expect(reprocessUnavailable(info(), true, true, false)).toBe("No stored image to reprocess in this set");
    expect(reprocessUnavailable(info(), true, false, true)).toBe("A review job is running; reprocess the record when it finishes");
  });
});

describe("a stored E of a run without an E-LUT (coordinator ruling on #774, option b)", () => {
  const noLut = info({ identity: identity({ emodulus_lut: { ...LUT, loaded: false, lut_id: null, revision: null, source: null, sha256: null, catalog_sha256: null, checksum_status: null } }) });
  const stored0 = row({ youngs_modulus: 0 });

  it("reads not computed, never 0.00 kPa, where the run recorded no LUT loaded", () => {
    expect(storedENotComputed(noLut)).toBe(true);
    expect(storedEText(stored0, noLut)).toBe("Not computed (no E-LUT loaded)");
    expect(fact(measurementFacts(stored0, noLut), "E (stored)")).toMatchObject({ value: "Not computed (no E-LUT loaded)", tone: "unknown" });
    const r: ReviewReprocess = {
      ok: true, message: "", settings: 0, used_recorded_settings: true, settings_label: "", core_version: "2.1.0", core_source: "bundled",
      pixel_to_micron: 0.25, emodulus_recomputed: false, emodulus_lut: null, emodulus_lut_sha256: null, stored: stored0, reprocessed: [stored0], match_index: 0, object_count_differs: false, truncated: false,
    };
    expect(reprocessComparison(r, noLut).find((l) => l.label === "E")?.stored).toBe("Not computed (no E-LUT loaded)");
  });

  it("keeps a legacy file's E as stored, and a loaded LUT's E as its value", () => {
    // No snapshot: emodulus_lut is null, nothing is guessed (no option c).
    const legacy = info({ identity: identity({ has_snapshot: false, snapshot_version: null, emodulus_lut: null }) });
    expect(storedENotComputed(legacy)).toBe(false);
    expect(storedEText(stored0, legacy)).toBe("0.00 kPa");
    expect(fact(measurementFacts(stored0, legacy), "E (stored)")?.value).toBe("0.00 kPa");
    expect(storedEText(row(), info({ identity: identity({ emodulus_lut: LUT }) }))).toBe("1.42 kPa");
    expect(storedEText(row({ youngs_modulus: Number.NaN }), legacy)).toBeNull();
    expect(storedENotComputed(null)).toBe(false);
  });
});
