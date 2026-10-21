// R3 Inspect's facts (#717 B3a, #269; B3c wires review ABI 3): what the
// record card, the linked records and the Stored card say about the selected
// record, from `fetch_review_info`, `fetch_review_rows`,
// `fetch_review_track_rows` and `review_reprocess_record`. Pure; tested in
// inspectModel.test.ts. Nothing here computes a value the file does not
// record: a missing fact is `null` (the kit shows it as missing), never a
// stand-in, and no record id is invented (files carry none: a record is
// named by its frame and object).
import type { Fact } from "@mib/ui";
import { metricNumber } from "../../metricFormat";
import {
  OVERLAY,
  type OverlayMode,
  type ReviewEModulusLut,
  type ReviewInfo,
  type ReviewReprocess,
  type ReviewRow,
  type ReviewRunIdentity,
  type ReviewTrackRows,
} from "../reviewBridge";

/** The record's name everywhere in Inspect (coordinator ruling on #717). A
 * raw recording's rows are frames, with no object. */
export function recordLabel(row: Pick<ReviewRow, "frame_index" | "object_id">, recording = false): string {
  return recording ? `frame ${row.frame_index}` : `frame ${row.frame_index} · object ${row.object_id}`;
}

/** How the record card shows the record's image. */
export type CellView = "raw" | "contour" | "mask" | "full";

export const CELL_VIEWS: { view: CellView; label: string }[] = [
  { view: "raw", label: "Raw" },
  { view: "contour", label: "Contour" },
  { view: "mask", label: "Mask" },
  { view: "full", label: "Full frame" },
];

/** The backend overlay a crop view asks for (Full frame uses the toolbar's choice). */
export function cropOverlay(view: Exclude<CellView, "full">): OverlayMode {
  return view === "raw" ? OVERLAY.None : view === "contour" ? OVERLAY.AllContour : OVERLAY.AllMask;
}

export interface CropRect {
  x: number;
  y: number;
  size: number;
}

/** The square around a bounding box (in image coordinates), with a margin,
 * inside the image (CellImage shows a square crop). Null without a bounding
 * box (raw recordings, files without metrics) or when the box lies wholly
 * outside the image. */
export function cropSquare(
  row: Pick<ReviewRow, "bbox_x" | "bbox_y" | "bbox_width" | "bbox_height">,
  width: number,
  height: number,
  margin = 1.6,
  minSize = 24,
): CropRect | null {
  const { bbox_x: bx, bbox_y: by, bbox_width: bw, bbox_height: bh } = row;
  if (![bx, by, bw, bh].every(Number.isFinite) || bw <= 0 || bh <= 0 || width <= 0 || height <= 0) return null;
  // A box wholly outside the image is not cut at the nearest edge (B3c).
  if (bx >= width || by >= height || bx + bw <= 0 || by + bh <= 0) return null;
  const size = Math.max(1, Math.min(width, height, Math.round(Math.max(minSize, Math.max(bw, bh) * margin))));
  const cx = bx + bw / 2;
  const cy = by + bh / 2;
  const clamp = (v: number, extent: number) => Math.min(Math.max(0, Math.round(v - size / 2)), extent - size);
  return { x: clamp(cx, width), y: clamp(cy, height), size };
}

/** The offset that places a stored bounding box or centroid in image
 * coordinates, from the file's object coordinate space (review ABI 3,
 * coordinator ruling on #717 of 2026-10-21): the ROI origin for
 * `roi_local`, nothing for `image`. Null when nothing may be placed: a raw
 * recording (no objects), an `unknown` space, or no info. */
export function objectOffset(info: ReviewInfo | null): { dx: number; dy: number } | null {
  if (!info || info.recording_file) return null;
  if (info.object_coordinates === "image") return { dx: 0, dy: 0 };
  if (info.object_coordinates === "roi_local") {
    const dx = info.object_origin_x;
    const dy = info.object_origin_y;
    return Number.isFinite(dx) && Number.isFinite(dy) ? { dx, dy } : null;
  }
  return null;
}

type Box = Pick<ReviewRow, "bbox_x" | "bbox_y" | "bbox_width" | "bbox_height">;

/** The record's stored box in image coordinates (null: it cannot be placed). */
export function placedBox(row: Box, info: ReviewInfo | null): Box | null {
  const offset = objectOffset(info);
  if (!offset) return null;
  return { bbox_x: row.bbox_x + offset.dx, bbox_y: row.bbox_y + offset.dy, bbox_width: row.bbox_width, bbox_height: row.bbox_height };
}

/** The crop square of a record in the frame packet: its box placed in image
 * coordinates, then `cropSquare` (kept inside the image). */
export function recordCrop(row: Box, info: ReviewInfo | null, width: number, height: number): CropRect | null {
  const box = placedBox(row, info);
  return box ? cropSquare(box, width, height) : null;
}

/** Why no cell crop is offered for a file whose space nothing decides. */
export const CROP_UNKNOWN_SPACE =
  "No cell crop: the file does not record whether its bounding boxes are relative to its ROI or to the image, and its masks do not tell";

/** Why a crop view cannot be chosen for this record, or null when it can. */
export function cropUnavailable(view: CellView, info: ReviewInfo | null, valid: boolean, row: ReviewRow | null): string | null {
  if (view === "full") return null;
  if (!info || info.recording_file) return "Recording files carry no objects";
  if (!objectOffset(info)) return CROP_UNKNOWN_SPACE;
  if (!row || cropSquare(row, 1e9, 1e9) === null) return "No bounding box is recorded for this record";
  if (view !== "raw" && !(valid ? info.valid_masks : info.invalid_masks).present) return "No masks are recorded in this set";
  return null;
}

const SPACE_SOURCE: Record<ReviewInfo["object_coordinates_source"], string> = {
  attribute: "recorded in the file (@object_coordinates)",
  snapshot: "decided by the run snapshot",
  mask_check: "decided by checking the stored masks",
  roi_origin: "the ROI starts at (0, 0) or has no area, so both placements are the same",
  "": "",
};

/** The file's object coordinate space and what decided it (Stored card). */
export function objectSpaceFact(info: ReviewInfo): Fact {
  const label = "Object coordinates";
  if (info.recording_file) return { label, value: "None (raw recording)", tone: "unknown" };
  const source = SPACE_SOURCE[info.object_coordinates_source] ?? "";
  const by = source ? ` · ${source}` : "";
  if (info.object_coordinates === "roi_local" && objectOffset(info))
    return { label, value: `ROI-local, placed at the ROI origin (${info.object_origin_x}, ${info.object_origin_y})${by}` };
  if (info.object_coordinates === "image") return { label, value: `image${by}` };
  return {
    label,
    value: "Unknown: the file does not record it and its masks do not tell, so no crop is cut and no record is reprocessed",
    tone: "warn",
  };
}

const saved = (present: boolean, detail?: string): Pick<Fact, "value" | "tone"> =>
  present ? { value: detail ? `Saved · ${detail}` : "Saved", tone: "confirmed" } : { value: "Not recorded", tone: "unknown" };

/** "What is recorded" for one record of a set, from dataset presence. */
export function recordedFacts(info: ReviewInfo, valid: boolean, row: ReviewRow | null): Fact[] {
  if (info.recording_file) {
    return [
      { label: "Frame image", ...saved(info.recorded_images.present, `${info.recorded_images.width}×${info.recorded_images.height}`) },
      { label: "Measurement", value: "Not recorded (raw recording)", tone: "unknown" },
      { label: "Mask / contour", value: "Not recorded (raw recording)", tone: "unknown" },
    ];
  }
  const images = valid ? info.valid_images : info.invalid_images;
  const masks = valid ? info.valid_masks : info.invalid_masks;
  const series = valid && info.has_series ? Number(info.series_count) : 0;
  return [
    { label: "Measurement", ...saved(row !== null) },
    { label: "Full frame", ...saved(images.present, `${images.width}×${images.height}`) },
    { label: "Mask / contour", ...saved(masks.present) },
    { label: "Image series", value: series > 0 ? `${series} images per record` : "None recorded", tone: series > 0 ? "confirmed" : "unknown" },
  ];
}

const NOT_RECORDED = { value: "Not recorded", tone: "unknown" } as const;

/** The selected record's stored values. A raw recording stores only the
 * frame index and timestamp: everything else is "Not recorded", never a 0
 * or "invalid" (#757 review). */
export function measurementFacts(row: ReviewRow, info: ReviewInfo): Fact[] {
  if (info.recording_file) {
    return [
      { label: "Frame", value: row.frame_index },
      { label: "Timestamp", value: `${row.timestamp_ns} ns` },
      ...["Area", "Deformability", "E (stored)", "Ring ratio", "Validation", "Track", "Objects in frame"].map((label) => ({ label, ...NOT_RECORDED })),
      { label: "Analysis", value: "None (raw recording)", tone: "unknown" as const },
    ];
  }
  const tracked = row.track_id >= 0;
  return [
    { label: "Area", value: `${metricNumber(row.area_um2, 2)} µm² · ${metricNumber(row.area, 1)} px²` },
    { label: "Deformability", value: metricNumber(row.deformability, 4) },
    { label: "E (stored)", value: Number.isFinite(row.youngs_modulus) ? `${metricNumber(row.youngs_modulus, 2)} kPa` : null },
    { label: "Ring ratio", value: metricNumber(row.ring_ratio, 3) },
    { label: "Timestamp", value: `${row.timestamp_ns} ns` },
    {
      label: "Validation",
      value: `${row.valid ? "valid" : "invalid"}${row.target_group ? " · target group" : ""}`,
      tone: row.valid ? "confirmed" : "warn",
    },
    {
      label: "Track",
      value: tracked
        ? `track ${row.track_id} · frames ${row.track_first_frame}–${row.track_last_frame} · ${row.track_observation_count} observations`
        : "Not tracked",
    },
    { label: "Objects in frame", value: String(row.object_count) },
    { label: "Analysis", value: info.has_core_identity ? `stored · core ${info.core_version}${info.core_source ? ` (${info.core_source})` : ""}` : "stored · core not recorded" },
  ];
}

/** The run's identity in one line under the record name (B1a identity). */
export function recordWhere(info: ReviewInfo, valid: boolean, position: number, total: number): string {
  const set = info.recording_file ? "frame" : valid ? "valid record" : "invalid record";
  const id = info.identity;
  return [
    `${set} ${position + 1} of ${total}`,
    id?.run_index !== null && id?.run_index !== undefined ? `run ${id.run_index}` : null,
    id?.sample_ref ? `sample ${id.sample_ref}` : null,
  ]
    .filter(Boolean)
    .join(" · ");
}

/** The E-LUT in a few words, or null when the identity records none. */
function lutWords(lut: ReviewEModulusLut | null): string | null {
  if (!lut) return null;
  if (!lut.loaded) return "none loaded (the run had no E values)";
  return [
    lut.lut_id ?? "id not recorded",
    lut.revision ? `revision ${lut.revision}` : null,
    lut.source,
    lut.sha256 ? `sha256 ${lut.sha256.slice(0, 12)}…` : null,
    lut.checksum_status ? `checksum ${lut.checksum_status}` : null,
  ]
    .filter(Boolean)
    .join(" · ");
}

/** The provenance line: experiment, snapshot, calibration, profile and E-LUT
 * (what the file declares; a v3 snapshot member it lacks reads "not
 * recorded"; a legacy file says it has no snapshot, nothing more). */
export function recordProvenance(info: ReviewInfo): string {
  const id = info.identity;
  if (!id) return "";
  const snapshot = id.has_snapshot;
  const lut = id.emodulus_lut ?? null;
  return [
    id.experiment_id ? `experiment ${id.experiment_id.slice(0, 8)}…` : null,
    snapshot ? `snapshot v${id.snapshot_version ?? "?"}` : id.snapshot_unreadable ? "snapshot unreadable" : "no run snapshot (legacy file)",
    id.calibration_record_id ? `calibration ${id.calibration_record_id}` : null,
    id.profile_id ? `profile ${id.profile_id}` : snapshot ? "profile not recorded" : null,
    lut ? (lut.loaded ? `E-LUT ${lut.lut_id ?? "id not recorded"}${lut.revision ? ` rev ${lut.revision}` : ""}` : "no E-LUT loaded") : snapshot ? "E-LUT not recorded" : null,
  ]
    .filter(Boolean)
    .join(" · ");
}

const NOT_RECORDED_FACT = { value: "Not recorded", tone: "unknown" } as const;

/** The run's E-LUT provenance (snapshot v3, review ABI 3). */
export function lutFact(id: ReviewRunIdentity | null | undefined): Fact {
  const lut = id?.emodulus_lut ?? null;
  if (!lut) return { label: "E-LUT", ...NOT_RECORDED_FACT };
  return { label: "E-LUT", value: lutWords(lut), tone: !lut.loaded ? "warn" : lut.checksum_status === "verified" ? "confirmed" : undefined };
}

/** The run's profile id: the only "method" a file records. */
export function profileFact(id: ReviewRunIdentity | null | undefined): Fact {
  return id?.profile_id ? { label: "Profile", value: id.profile_id } : { label: "Profile", ...NOT_RECORDED_FACT };
}

/** The Stored card: what produced the stored results, and how reprocessing works. */
export function storedFacts(info: ReviewInfo): Fact[] {
  const id = info.identity;
  const accounting = info.accounting_summary.replace(/^\s*·\s*/, "");
  return [
    {
      // Neutral (#757 review): a regenerated file records the core that
      // regenerated it, and no marker tells it from a capture yet (#754).
      label: "Stored results",
      value: info.has_core_identity ? `core ${info.core_version}${info.core_source ? ` (${info.core_source})` : ""} recorded in this file` : null,
    },
    { label: "Run snapshot", value: id?.has_snapshot ? `v${id.snapshot_version ?? "?"}${id.run_id ? ` · run ${id.run_id}` : ""}` : id?.snapshot_unreadable ? "Unreadable" : null },
    {
      label: "Calibration used",
      value: `${id?.calibration_record_id ? `${id.calibration_record_id} · ` : ""}px→µm ${metricNumber(info.pixel_to_micron, 4)} ${info.pixel_to_micron_from_file ? "(recorded)" : "(fallback: the file records none)"}`,
      tone: info.pixel_to_micron_from_file ? undefined : "warn",
    },
    profileFact(id),
    lutFact(id),
    objectSpaceFact(info),
    { label: "Run accounting", value: accounting || null },
    { label: "Reprocessing", value: "Reprocessing writes a new file; this view lists no history" },
  ];
}

/** What the linked records show above their rows. */
export function trackSummary(t: ReviewTrackRows, trackId: number, valid: boolean): string {
  const set = valid ? "valid" : "invalid";
  const total = t.total;
  const shown = t.rows.length;
  return t.truncated
    ? `Track ${trackId}: ${total} records in the ${set} set; the first ${shown} are shown (at most 256 per request)`
    : `Track ${trackId}: ${total} ${total === "1" ? "record" : "records"} in the ${set} set`;
}

/** Why "Reprocess this record" cannot be sent, or null when it can. The
 * backend refuses the same cases; these are said before a call is made. */
export function reprocessUnavailable(
  info: ReviewInfo | null,
  selected: boolean,
  imagesMissing: boolean,
  jobsBusy: boolean,
): string | null {
  if (!info || !selected) return "Select a record to reprocess";
  if (info.recording_file) return "Recording files carry no objects";
  if (info.object_coordinates !== "roi_local" && info.object_coordinates !== "image")
    return "Not offered: the file does not record whether its bounding boxes are relative to its ROI or to the image, and its masks do not tell, so the record cannot be matched";
  if (imagesMissing) return "No stored image to reprocess in this set";
  if (jobsBusy) return "A review job is running; reprocess the record when it finishes";
  return null;
}

export interface CompareLine {
  label: string;
  stored: string | null;
  reprocessed: string | null;
}

const num = (v: number | null | undefined, digits: number): string | null =>
  typeof v === "number" && Number.isFinite(v) ? v.toFixed(digits) : null;

/** Stored next to reprocessed, value by value. The stored box and centroid
 * are placed in image coordinates (the reprocessed ones are reported so);
 * an E not recomputed reads so, never 0. */
export function reprocessComparison(r: ReviewReprocess, info: ReviewInfo): CompareLine[] {
  const s = r.stored;
  if (!s) return [];
  const m = r.match_index !== null ? (r.reprocessed[r.match_index] ?? null) : null;
  const offset = objectOffset(info) ?? { dx: 0, dy: 0 };
  const box = (row: ReviewRow, dx: number, dy: number) =>
    `${num(row.bbox_x + dx, 0)}, ${num(row.bbox_y + dy, 0)} · ${num(row.bbox_width, 0)}×${num(row.bbox_height, 0)}`;
  const centre = (row: ReviewRow, dx: number, dy: number) => `${num(row.centroid_x + dx, 1)}, ${num(row.centroid_y + dy, 1)}`;
  const validation = (row: ReviewRow) => `${row.valid ? "valid" : "invalid"}${row.target_group ? " · target group" : ""}`;
  const found = r.truncated ? `more than ${r.reprocessed.length}` : String(r.reprocessed.length);
  const e = (row: ReviewRow | null, recomputed: boolean): string | null => {
    if (!row) return null;
    if (!recomputed) return "not recomputed (no E-LUT loaded)";
    return num(row.youngs_modulus, 2) !== null ? `${num(row.youngs_modulus, 2)} kPa` : null;
  };
  return [
    { label: "Area (px²)", stored: num(s.area, 1), reprocessed: m ? num(m.area, 1) : null },
    { label: "Area (µm²)", stored: num(s.area_um2, 2), reprocessed: m ? num(m.area_um2, 2) : null },
    { label: "Deformability", stored: num(s.deformability, 4), reprocessed: m ? num(m.deformability, 4) : null },
    { label: "Area ratio", stored: num(s.area_ratio, 3), reprocessed: m ? num(m.area_ratio, 3) : null },
    { label: "Ring ratio", stored: num(s.ring_ratio, 3), reprocessed: m ? num(m.ring_ratio, 3) : null },
    { label: "E", stored: num(s.youngs_modulus, 2) !== null ? `${num(s.youngs_modulus, 2)} kPa` : null, reprocessed: e(m, r.emodulus_recomputed) },
    { label: "Validation", stored: validation(s), reprocessed: m ? validation(m) : null },
    { label: "Box (image x, y · w×h)", stored: box(s, offset.dx, offset.dy), reprocessed: m ? box(m, 0, 0) : null },
    { label: "Centroid (image)", stored: centre(s, offset.dx, offset.dy), reprocessed: m ? centre(m, 0, 0) : null },
    { label: "Objects in frame", stored: s.object_count > 0 ? String(s.object_count) : null, reprocessed: found },
  ];
}

/** One line on the match: which found object is compared, or why none is. */
export function reprocessMatchText(r: ReviewReprocess): string {
  const n = r.reprocessed.length;
  const found = r.truncated ? `more than ${n} objects` : n === 1 ? "1 object" : `${n} objects`;
  if (n === 0) return "No match: the kernel found no object in the record's stored image";
  if (r.match_index === null) return `No match: the kernel found ${found}, none with its centroid inside the stored box`;
  return `Matched object ${r.match_index + 1} of ${found} found (centroid nearest the stored one, inside the stored box)`;
}
