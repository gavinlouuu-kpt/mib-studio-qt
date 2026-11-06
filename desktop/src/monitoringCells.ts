// Live cell images for the Monitoring / Live views (bridge ABI 41, #716 A6a; ABI next (#880, set at merge), #866: the
// invalid strip and the rejection reasons).
//
// The backend owns the copies: a bounded strip of the most recently detected valid cells, a strip
// of the most recent invalid ones (each with why it failed validation) and the operator's pins
// (bridge-contract.json `monitoring_cells`).
// This module only types and checks what the commands return. Pixels never
// travel in JSON: `bridge.fetchMonitoringCellImage` pulls a frame packet
// (pull kind 5 = image, 6 = mask) by the cell's identity.
import {
  MONITORING_CELLS, MONITORING_CELL_ERROR_CODES, MONITORING_CELL_PIN_OUTCOMES, MONITORING_CELL_REASON_CODES,
} from "./bridgeContract";
import { decimalU64 } from "./framePacket";

/** A monitoring row's identity: the run active when its frame entered processing (0 = outside a
 *  run), the capture its frame entered with (frame indices restart when the capture changes; the
 *  epoch never repeats; "0" = a frame that crossed a capture change, never a cell), its frame
 *  index and object id. u64 values are canonical decimal strings. */
export interface MonitoringCellKey {
  start_generation: string;
  capture_epoch: string;
  frame_index: string;
  object_id: number;
}

/** What the caller may pass as an identity: a listing cell, a snapshot row (whose frame_index is a
 *  JSON number) or the parts by hand. */
export interface MonitoringCellKeyInput {
  start_generation: string | number | bigint;
  capture_epoch: string | number | bigint;
  frame_index: string | number | bigint;
  object_id: number;
}

export interface MonitoringCellImageInfo {
  width: number;
  height: number;
  crop_x: number;
  crop_y: number;
  source_width: number;
  source_height: number;
  has_mask: boolean;
  bytes: string;
}

export type MonitoringCellReasonCode = (typeof MONITORING_CELL_REASON_CODES)[number];

/** One reason an invalid cell failed validation (the shared classifier's, the one the live
 *  invalid-reason histogram uses): `value` is what was measured and [min, max] the configured range
 *  it was held against. null where a reason has none (no_contour, border; no minimum for
 *  area_ratio; no band for wall when none is configured) or the number is not finite. */
export interface MonitoringCellReason {
  code: MonitoringCellReasonCode;
  value: number | null;
  min: number | null;
  max: number | null;
}

/** One cell: the row's measurements as recorded, and either the crop's geometry or the plain reason
 *  it has none. Non-finite numbers arrive as null. Centroid, bbox and crop_x / crop_y are in the
 *  row image's coordinates (the processing ROI, source_width x source_height) in every mode. */
export interface MonitoringCell extends MonitoringCellKey {
  timestamp_ns: string;
  valid: boolean;
  target_group: boolean;
  object_count: number;
  track_id: number;
  centroid_x: number | null;
  centroid_y: number | null;
  bbox: { x: number | null; y: number | null; w: number | null; h: number | null };
  area: number | null;
  deformability: number | null;
  area_ratio: number | null;
  ring_ratio: number | null;
  youngs_modulus: number | null;
  pixel_to_micron: number | null;
  /** Why an invalid cell failed validation, in the classifier's order; [] for a valid cell (and for
   *  a pin whose row only the bounded index still describes). */
  reasons: MonitoringCellReason[];
  image: MonitoringCellImageInfo | null;
  no_image_reason: string | null;
}

/** The last capture change: pins belong to the capture that made them, so at that moment the strip
 *  was cleared and the pins of the capture that ended kept their measurements only (a run start
 *  drops the pins instead). */
export interface MonitoringCaptureChange {
  /** The capture that began. */
  capture_epoch: string;
  /** Why, in plain words ("the camera stopped", "realtime processing stopped", ...). */
  reason: string;
  /** Pins of the capture that ended, now listed without an image. */
  pins_ended: number;
  /** Pins removed (a run start). */
  pins_dropped: number;
  strip_dropped: number;
  /** Cells the invalid strip held when the capture changed. */
  invalid_strip_dropped: number;
}

export interface MonitoringCellsListing {
  valid: true;
  /** The latest run started ("0" before the first); pins belong to it. */
  scope_start_generation: string;
  /** The strip's context: the active run, or "0" outside a run ... */
  ring_start_generation: string;
  /** The current capture: every strip cell is of it, and a pin must name it (else CaptureMismatch).
   *  A pin of an ended capture is listed with its measurements only (image null, the reason said). */
  ring_capture_epoch: string;
  /** Bumps whenever the strip or the pins change. */
  revision: string;
  /** The most recent valid cells, oldest -> newest (at most caps.strip_max_cells). */
  strip: MonitoringCell[];
  /** The most recent invalid cells, oldest -> newest (at most caps.invalid_strip_max_cells), each
   *  with its reasons. */
  invalid_strip: MonitoringCell[];
  /** In pin order; a pin whose capture ended has no image until it is unpinned. */
  pins: MonitoringCell[];
  /** null when the capture has not changed since the backend started. */
  last_capture_change: MonitoringCaptureChange | null;
  /** Counts bounded by a cap are numbers; bytes and counters are decimal strings. */
  stats: Record<string, number | string>;
  /** The backend's caps (bridge-contract.json `monitoring_cells`). */
  caps: Record<string, number>;
}

export type MonitoringCellsReply = MonitoringCellsListing | { valid: false };

export type MonitoringCellPinOutcome = (typeof MONITORING_CELL_PIN_OUTCOMES)[keyof typeof MONITORING_CELL_PIN_OUTCOMES];

export interface MonitoringCellPinReply {
  /** Pinned, PinnedWithoutImage or AlreadyPinned. CaptureMismatch: the identity names an earlier
   *  capture (the camera mode or frame buffer changed); refresh the listing and pick again. */
  ok: boolean;
  /** Absent when the backend never evaluated the pin (e.g. before init). */
  outcome?: MonitoringCellPinOutcome;
  message: string;
  /** The pin, or null when refused. */
  cell: MonitoringCell | null;
  revision?: string;
}

function invalid(why: string): never {
  throw new Error(`MONITORING_CELLS_INVALID: ${why}`);
}

const isObject = (v: unknown): v is Record<string, unknown> => typeof v === "object" && v !== null && !Array.isArray(v);
const numberOrNull = (v: unknown, key: string): number | null => {
  if (v === null) return null;
  if (typeof v !== "number" || !Number.isFinite(v)) invalid(`${key} is not a number`);
  return v;
};
const integer = (v: unknown, key: string): number => {
  if (typeof v !== "number" || !Number.isInteger(v)) invalid(`${key} is not an integer`);
  return v;
};
const decimal = (v: unknown, key: string): string => {
  if (typeof v !== "string") invalid(`${key} is not a decimal string`);
  try {
    return decimalU64(v);
  } catch {
    return invalid(`${key} is not a canonical decimal u64`);
  }
};
const boolean = (v: unknown, key: string): boolean => {
  if (typeof v !== "boolean") invalid(`${key} is not a boolean`);
  return v;
};

/** The identity arguments a command takes: canonical decimal strings and an i32 object id. Throws
 *  INVALID_U64 / INVALID_OBJECT_ID before anything is sent. */
export function cellKeyArgs(key: MonitoringCellKeyInput, generationName = "startGeneration") {
  if (!Number.isInteger(key.object_id) || key.object_id < -2147483648 || key.object_id > 2147483647)
    throw new Error("INVALID_OBJECT_ID: expected a 32-bit integer");
  return {
    [generationName]: decimalU64(key.start_generation),
    captureEpoch: decimalU64(key.capture_epoch),
    frameIndex: decimalU64(key.frame_index),
    objectId: key.object_id,
  } as Record<string, string | number>;
}

/** A stable string for client-side caches and React keys. */
export function cellKeyString(key: MonitoringCellKeyInput): string {
  return `${decimalU64(key.start_generation)}:${decimalU64(key.capture_epoch)}:${decimalU64(key.frame_index)}:${key.object_id}`;
}

const REASON_CODES = new Set<string>(MONITORING_CELL_REASON_CODES);

function decodeReasons(raw: unknown): MonitoringCellReason[] {
  if (!Array.isArray(raw)) return invalid("reasons is not an array");
  if (raw.length > MONITORING_CELL_REASON_CODES.length) invalid("more reasons than reason codes");
  const seen = new Set<string>();
  return raw.map((r, i) => {
    if (!isObject(r)) return invalid(`reasons[${i}] is not an object`);
    if (typeof r.code !== "string" || !REASON_CODES.has(r.code)) return invalid(`reasons[${i}].code is not a contract reason code`);
    if (seen.has(r.code)) invalid(`reasons[${i}].code ${r.code} is listed twice`);
    seen.add(r.code);
    return {
      code: r.code as MonitoringCellReasonCode,
      value: numberOrNull(r.value, `reasons[${i}].value`),
      min: numberOrNull(r.min, `reasons[${i}].min`),
      max: numberOrNull(r.max, `reasons[${i}].max`),
    };
  });
}

export function decodeMonitoringCell(raw: unknown): MonitoringCell {
  if (!isObject(raw)) invalid("cell is not an object");
  const bbox = isObject(raw.bbox) ? raw.bbox : invalid("bbox");
  let image: MonitoringCellImageInfo | null = null;
  if (raw.image !== null) {
    if (!isObject(raw.image)) invalid("image is not an object");
    const i = raw.image;
    image = {
      width: integer(i.width, "image.width"),
      height: integer(i.height, "image.height"),
      crop_x: integer(i.crop_x, "image.crop_x"),
      crop_y: integer(i.crop_y, "image.crop_y"),
      source_width: integer(i.source_width, "image.source_width"),
      source_height: integer(i.source_height, "image.source_height"),
      has_mask: boolean(i.has_mask, "image.has_mask"),
      bytes: decimal(i.bytes, "image.bytes"),
    };
    if (image.width < 1 || image.height < 1 || image.width > MONITORING_CELLS.crop_max_side
        || image.height > MONITORING_CELLS.crop_max_side) invalid("image size outside the crop bounds");
  }
  const reason = raw.no_image_reason;
  if (reason !== null && typeof reason !== "string") invalid("no_image_reason");
  const reasons = decodeReasons(raw.reasons);
  const valid = boolean(raw.valid, "valid");
  if (valid && reasons.length > 0) invalid("a valid cell lists reasons");
  if ((image === null) === (reason === null)) invalid("exactly one of image and no_image_reason");
  return {
    start_generation: decimal(raw.start_generation, "start_generation"),
    capture_epoch: decimal(raw.capture_epoch, "capture_epoch"),
    frame_index: decimal(raw.frame_index, "frame_index"),
    object_id: integer(raw.object_id, "object_id"),
    timestamp_ns: decimal(raw.timestamp_ns, "timestamp_ns"),
    valid,
    target_group: boolean(raw.target_group, "target_group"),
    object_count: integer(raw.object_count, "object_count"),
    track_id: integer(raw.track_id, "track_id"),
    centroid_x: numberOrNull(raw.centroid_x, "centroid_x"),
    centroid_y: numberOrNull(raw.centroid_y, "centroid_y"),
    bbox: {
      x: numberOrNull(bbox.x, "bbox.x"),
      y: numberOrNull(bbox.y, "bbox.y"),
      w: numberOrNull(bbox.w, "bbox.w"),
      h: numberOrNull(bbox.h, "bbox.h"),
    },
    area: numberOrNull(raw.area, "area"),
    deformability: numberOrNull(raw.deformability, "deformability"),
    area_ratio: numberOrNull(raw.area_ratio, "area_ratio"),
    ring_ratio: numberOrNull(raw.ring_ratio, "ring_ratio"),
    youngs_modulus: numberOrNull(raw.youngs_modulus, "youngs_modulus"),
    pixel_to_micron: numberOrNull(raw.pixel_to_micron, "pixel_to_micron"),
    reasons,
    image,
    no_image_reason: reason as string | null,
  };
}

/** fetch_monitoring_cells: checked, bounded by the contract caps; throws MONITORING_CELLS_INVALID. */
export function decodeMonitoringCells(raw: unknown): MonitoringCellsReply {
  if (!isObject(raw)) invalid("listing is not an object");
  if (raw.valid === false) return { valid: false };
  if (raw.valid !== true) invalid("valid");
  if (!Array.isArray(raw.strip) || !Array.isArray(raw.invalid_strip) || !Array.isArray(raw.pins))
    invalid("strip / invalid_strip / pins are not arrays");
  if (raw.strip.length > MONITORING_CELLS.strip_max_cells) invalid("strip over its cap");
  if (raw.invalid_strip.length > MONITORING_CELLS.invalid_strip_max_cells) invalid("invalid_strip over its cap");
  if (raw.pins.length > MONITORING_CELLS.max_pins) invalid("pins over their cap");
  if (!isObject(raw.stats) || !isObject(raw.caps)) invalid("stats / caps");
  let change: MonitoringCaptureChange | null = null;
  if (raw.last_capture_change !== null) {
    if (!isObject(raw.last_capture_change)) invalid("last_capture_change");
    const c = raw.last_capture_change;
    if (typeof c.reason !== "string") invalid("last_capture_change.reason");
    change = {
      capture_epoch: decimal(c.capture_epoch, "last_capture_change.capture_epoch"),
      reason: c.reason,
      pins_ended: integer(c.pins_ended, "last_capture_change.pins_ended"),
      pins_dropped: integer(c.pins_dropped, "last_capture_change.pins_dropped"),
      strip_dropped: integer(c.strip_dropped, "last_capture_change.strip_dropped"),
      invalid_strip_dropped: integer(c.invalid_strip_dropped, "last_capture_change.invalid_strip_dropped"),
    };
  }
  const ringCaptureEpoch = decimal(raw.ring_capture_epoch, "ring_capture_epoch");
  const strip = raw.strip.map(decodeMonitoringCell);
  const invalidStrip = raw.invalid_strip.map(decodeMonitoringCell);
  const pins = raw.pins.map(decodeMonitoringCell);
  // The strip holds cells of the current capture only, and a pin of another capture has no image:
  // anything else is a contract breach.
  if (strip.some(c => c.capture_epoch !== ringCaptureEpoch)) invalid("a strip cell of another capture is listed");
  if (invalidStrip.some(c => c.capture_epoch !== ringCaptureEpoch)) invalid("an invalid strip cell of another capture is listed");
  // Each strip holds one kind: a valid cell in the invalid strip (or the reverse) is a contract breach.
  if (strip.some(c => !c.valid)) invalid("an invalid cell is listed in the valid strip");
  if (invalidStrip.some(c => c.valid)) invalid("a valid cell is listed in the invalid strip");
  if (pins.some(c => c.capture_epoch !== ringCaptureEpoch && c.image !== null))
    invalid("a pin of an ended capture is listed with an image");
  return {
    valid: true,
    scope_start_generation: decimal(raw.scope_start_generation, "scope_start_generation"),
    ring_start_generation: decimal(raw.ring_start_generation, "ring_start_generation"),
    ring_capture_epoch: ringCaptureEpoch,
    revision: decimal(raw.revision, "revision"),
    strip,
    invalid_strip: invalidStrip,
    pins,
    last_capture_change: change,
    stats: raw.stats as Record<string, number | string>,
    caps: raw.caps as Record<string, number>,
  };
}

/** The plain reason when `error` (a rejected fetchMonitoringCellImage) is the backend refusing an
 *  identity of another capture than the current one (bridge-contract.json
 *  `monitoring_cells.error_codes.capture_mismatch`); null for any other error. The cell is gone
 *  for good: refresh the listing rather than retry. */
export function monitoringCellCaptureMismatch(error: unknown): string | null {
  const text = typeof error === "string" ? error : error instanceof Error ? error.message : null;
  const prefix = `${MONITORING_CELL_ERROR_CODES.capture_mismatch}: `;
  return text !== null && text.startsWith(prefix) ? text.slice(prefix.length) : null;
}

const PIN_OUTCOMES = new Set<number>(Object.values(MONITORING_CELL_PIN_OUTCOMES));

/** monitoring_cell_pin's reply; an unknown outcome is rejected (contract: reject and reconcile). */
export function decodeMonitoringCellPinReply(raw: unknown): MonitoringCellPinReply {
  if (!isObject(raw)) invalid("pin reply is not an object");
  const ok = boolean(raw.ok, "ok");
  if (typeof raw.message !== "string") invalid("message");
  let outcome: MonitoringCellPinOutcome | undefined;
  if (raw.outcome !== undefined) {
    if (typeof raw.outcome !== "number" || !PIN_OUTCOMES.has(raw.outcome)) invalid("unknown pin outcome");
    outcome = raw.outcome as MonitoringCellPinOutcome;
  }
  const cell = raw.cell === null || raw.cell === undefined ? null : decodeMonitoringCell(raw.cell);
  return {
    ok,
    ...(outcome !== undefined ? { outcome } : {}),
    message: raw.message,
    cell,
    ...(raw.revision !== undefined ? { revision: decimal(raw.revision, "revision") } : {}),
  };
}

export function pinOutcomeName(outcome: MonitoringCellPinOutcome | undefined): string {
  if (outcome === undefined) return "unknown";
  for (const [name, value] of Object.entries(MONITORING_CELL_PIN_OUTCOMES)) if (value === outcome) return name;
  return "unknown";
}
