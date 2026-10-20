// R3 Inspect's facts (#717 B3a, #269): what the record card and the Stored
// card say about the selected record, from the commands the review contract
// already has (`fetch_review_info`, `fetch_review_rows`). Pure; tested in
// inspectModel.test.ts. Nothing here computes a value the file does not
// record: a missing fact is `null` (the kit shows it as missing), never a
// stand-in, and no record id is invented (files carry none: a record is
// named by its frame and object).
import type { Fact } from "@mib/ui";
import { metricNumber } from "../../metricFormat";
import { OVERLAY, type OverlayMode, type ReviewInfo, type ReviewRow } from "../reviewBridge";

/** The record's name everywhere in Inspect (coordinator ruling on #717). */
export function recordLabel(row: Pick<ReviewRow, "frame_index" | "object_id">): string {
  return `frame ${row.frame_index} · object ${row.object_id}`;
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

/** The square around a record's bounding box, with a margin, inside the
 * image (CellImage shows a square crop). Null without a bounding box: the
 * record cannot be cropped (raw recordings, files without metrics). */
export function cropSquare(
  row: Pick<ReviewRow, "bbox_x" | "bbox_y" | "bbox_width" | "bbox_height">,
  width: number,
  height: number,
  margin = 1.6,
  minSize = 24,
): CropRect | null {
  const { bbox_x: bx, bbox_y: by, bbox_width: bw, bbox_height: bh } = row;
  if (![bx, by, bw, bh].every(Number.isFinite) || bw <= 0 || bh <= 0 || width <= 0 || height <= 0) return null;
  const size = Math.max(1, Math.min(width, height, Math.round(Math.max(minSize, Math.max(bw, bh) * margin))));
  const cx = bx + bw / 2;
  const cy = by + bh / 2;
  const clamp = (v: number, extent: number) => Math.min(Math.max(0, Math.round(v - size / 2)), extent - size);
  return { x: clamp(cx, width), y: clamp(cy, height), size };
}

/** Why a crop view cannot be chosen for this record, or null when it can. */
export function cropUnavailable(view: CellView, info: ReviewInfo | null, valid: boolean, row: ReviewRow | null): string | null {
  if (view === "full") return null;
  if (!info || info.recording_file) return "Recording files carry no objects";
  if (!row || cropSquare(row, 1e9, 1e9) === null) return "No bounding box is recorded for this record";
  if (view !== "raw" && !(valid ? info.valid_masks : info.invalid_masks).present) return "No masks are recorded in this set";
  return null;
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

/** The selected record's stored values. */
export function measurementFacts(row: ReviewRow, info: ReviewInfo): Fact[] {
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

/** The provenance line: experiment, snapshot, calibration (what the file declares). */
export function recordProvenance(info: ReviewInfo): string {
  const id = info.identity;
  if (!id) return "";
  return [
    id.experiment_id ? `experiment ${id.experiment_id.slice(0, 8)}…` : null,
    id.has_snapshot ? `snapshot v${id.snapshot_version ?? "?"}` : id.snapshot_unreadable ? "snapshot unreadable" : "no run snapshot (legacy file)",
    id.calibration_record_id ? `calibration ${id.calibration_record_id}` : null,
  ]
    .filter(Boolean)
    .join(" · ");
}

/** The Stored card: what produced the stored results, and how reprocessing works. */
export function storedFacts(info: ReviewInfo): Fact[] {
  const id = info.identity;
  const accounting = info.accounting_summary.replace(/^\s*·\s*/, "");
  return [
    {
      label: "Stored results",
      value: info.has_core_identity ? `core ${info.core_version}${info.core_source ? ` · ${info.core_source}` : ""} · at acquisition` : null,
    },
    { label: "Run snapshot", value: id?.has_snapshot ? `v${id.snapshot_version ?? "?"}${id.run_id ? ` · run ${id.run_id}` : ""}` : id?.snapshot_unreadable ? "Unreadable" : null },
    {
      label: "Calibration used",
      value: `${id?.calibration_record_id ? `${id.calibration_record_id} · ` : ""}px→µm ${metricNumber(info.pixel_to_micron, 4)} ${info.pixel_to_micron_from_file ? "(recorded)" : "(fallback: the file records none)"}`,
      tone: info.pixel_to_micron_from_file ? undefined : "warn",
    },
    { label: "E-LUT", value: "Not available", tone: "unknown" },
    { label: "Run accounting", value: accounting || null },
    { label: "Reprocessing", value: "Reprocessing writes a new file; this view lists no history" },
  ];
}
