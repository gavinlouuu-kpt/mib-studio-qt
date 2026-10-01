// Typed client for the review bridge commands (src-tauri/src/review.rs;
// ADR 0008, plan 2026-10-01-standalone-review-app). The one review surface
// for both products: MIB Studio's Review tab and YOFO Review call these and
// nothing else for review. Images arrive as binary frame packets (Mono8 or
// RGB8 when the backend composed an overlay / ROI); everything else is
// lossless JSON (u64 as decimal strings).
import { invoke } from "@tauri-apps/api/core";
import { FRAME_PACKET, REVIEW_PIXEL_FORMATS } from "../bridgeContract";
import { decodeFramePacket, decimalU64, type FramePacket } from "../framePacket";

export interface ReviewDatasetInfo {
  present: boolean;
  count: string;
  height: number;
  width: number;
  channels: number;
}

export interface ReviewInfo {
  valid: boolean;
  file_open: boolean;
  file_path: string;
  recording_file: boolean;
  start_time_ns: string;
  end_time_ns: string;
  total_valid: string;
  total_invalid: string;
  filtered_frames: string;
  roi_x: number;
  roi_y: number;
  roi_w: number;
  roi_h: number;
  has_background: boolean;
  has_core_identity: boolean;
  core_version: string;
  core_source: string;
  core_release_tag: string;
  valid_images: ReviewDatasetInfo;
  invalid_images: ReviewDatasetInfo;
  valid_masks: ReviewDatasetInfo;
  invalid_masks: ReviewDatasetInfo;
  recorded_images: ReviewDatasetInfo;
  has_series: boolean;
  series_count: string;
  multi_image_enabled: boolean;
  multi_image_count: string;
  has_accounting: boolean;
  /** Contract run_completion_states value. */
  accounting_completion: number;
  accounting_reconciled: boolean;
  accounting_empty: string;
  accounting_rejected: string;
  accounting_processing_failed: string;
  accounting_store_loss: string;
  accounting_persisted: string;
  accounting_admitted: string;
  accounting_persistence_failed: string;
  accounting_summary: string;
  /** TD-17: recorded factor when `pixel_to_micron_from_file`, else fallback. */
  pixel_to_micron: number;
  pixel_to_micron_from_file: boolean;
  kde_analysis_json: string;
  kde_live_json: string;
}

export interface ReviewRow {
  frame_index: string;
  timestamp_ns: string;
  valid: boolean;
  target_group: boolean;
  touches_border: boolean;
  has_single_inner_contour: boolean;
  in_range: boolean;
  in_channel: boolean;
  inner_contour_count: number;
  object_id: number;
  object_count: number;
  track_id: number;
  track_first_frame: string;
  track_last_frame: string;
  track_observation_count: number;
  bbox_x: number;
  bbox_y: number;
  bbox_width: number;
  bbox_height: number;
  centroid_x: number;
  centroid_y: number;
  area: number;
  area_um2: number;
  deformability: number;
  area_ratio: number;
  ring_ratio: number;
  laplacian_variance: number | null;
  youngs_modulus: number;
  brightness_q1: number;
  brightness_q2: number;
  brightness_q3: number;
  brightness_q4: number;
}

export interface ReviewRows {
  valid: boolean;
  total: string;
  offset: string;
  rows: ReviewRow[];
}

export interface ReviewScatter {
  valid: boolean;
  pixel_to_micron: number;
  frame_index: string[];
  valid_position: string[];
  area_um2: number[];
  deformability: number[];
  target_group: number[];
}

export interface ReviewEvent {
  operation_id: string;
  /** Contract review_operation_kinds value. */
  kind: number;
  /** Contract operation_states value. */
  state: number;
  progress: string;
  total: string;
  message: string;
}

export interface ReviewDensity {
  valid: boolean;
  ready: boolean;
  levels: number[];
  level_count: number;
  bandwidth_factor: number;
  core_fraction: number;
  computed_record_json: string;
}

export interface ChartSnapshot {
  name: string;
  encoded: number[];
}

export interface SeriesRange {
  exportSeries: boolean;
  /** 0-based inclusive; `end` empty = to the last image. */
  start: number | string;
  end: number | string | "";
}

export const ALL_SERIES: SeriesRange = { exportSeries: true, start: 0, end: "" };

/** Contract review_regenerate_sources. */
export const REGENERATE_SOURCE = { CurrentValid: 0, CurrentInvalid: 1, WholeFile: 2, Avi: 3, Folder: 4 } as const;

export interface ReviewCmdResult {
  ok: boolean;
  command: number;
  message: string;
  operation_id: string;
}

/** Contract review_image_datasets. */
export const REVIEW_DATASET = { ValidImage: 0, InvalidImage: 1, RecordedImage: 2, ValidMask: 3, InvalidMask: 4 } as const;
/** Contract overlay_modes. */
export const OVERLAY = { None: 0, AllContour: 1, OuterInnerColorCoded: 2, AllMask: 3, FilteredMask: 4 } as const;
export type OverlayMode = (typeof OVERLAY)[keyof typeof OVERLAY];

export const PULL_REVIEW = FRAME_PACKET.pull_kinds.review;
export const PULL_REVIEW_THUMBNAILS = FRAME_PACKET.pull_kinds.review_thumbnails;
export const PULL_REVIEW_SERIES = FRAME_PACKET.pull_kinds.review_series;

// Local generation guard: a reply that started before a file change is
// retired by the FramePullScheduler's epochs; this only rejects replies that
// straddle an open/close within one pull.
let generation = 0;

async function pull(command: string, kind: number, args: Record<string, unknown>): Promise<FramePacket> {
  const g = generation;
  const buffer = await invoke<ArrayBuffer>(command, args);
  if (g !== generation) throw new Error("FRAME_REPLY_STALE");
  return decodeFramePacket(buffer, kind);
}

async function command(name: string, args?: Record<string, unknown>): Promise<ReviewCmdResult> {
  const r = await invoke<ReviewCmdResult>(name, args);
  return { ok: !!r.ok, command: Number(r.command), message: String(r.message ?? ""), operation_id: decimalU64(r.operation_id ?? "0") };
}

export const reviewBridge = {
  abiVersion: () => invoke<number>("review_abi_version"),
  open: async (path: string) => {
    generation++;
    try {
      return await command("review_open", { path });
    } finally {
      generation++;
    }
  },
  close: async () => {
    generation++;
    try {
      return await command("review_close");
    } finally {
      generation++;
    }
  },
  setPixelToMicron: (factor: number) => invoke<void>("review_set_pixel_to_micron", { factor }),
  info: () => invoke<ReviewInfo>("fetch_review_info"),
  rows: (valid: boolean, offset: number | string, count: number) =>
    invoke<ReviewRows>("fetch_review_rows", { valid, offset: decimalU64(offset), count }),
  frame: (dataset: number, index: number | string, overlay: OverlayMode = OVERLAY.None, roiOverlay = false) =>
    pull("fetch_review_frame", PULL_REVIEW, { dataset, index: decimalU64(index), overlay, roiOverlay }),
  seriesCount: async (index: number | string) =>
    Number(await invoke<string>("fetch_review_series_count", { index: decimalU64(index) })),
  seriesFrame: (index: number | string, k: number | string, overlay: OverlayMode = OVERLAY.None, roiOverlay = false) =>
    pull("fetch_review_series_packet", PULL_REVIEW_SERIES, { index: decimalU64(index), k: decimalU64(k), overlay, roiOverlay }),
  thumbnails: (valid: boolean, offset: number | string, count: number, size: number, overlay: OverlayMode = OVERLAY.None, roiOverlay = false) =>
    pull("fetch_review_thumbnails_packet", PULL_REVIEW_THUMBNAILS, { valid, offset: decimalU64(offset), count, size, overlay, roiOverlay }),
  scatter: () => invoke<ReviewScatter>("fetch_review_scatter"),
  saveCoreRecord: (json: string, overwrite: boolean) => command("review_save_core_record", { json, overwrite }),
  // Jobs (tracked operations; outcomes through pollEvents).
  exportMetrics: (outputPath: string) => command("review_export_metrics", { outputPath }),
  exportAll: (outputRoot: string, series: SeriesRange = ALL_SERIES, charts: ChartSnapshot[] = []) =>
    command("review_export_all", {
      outputRoot,
      exportSeries: series.exportSeries,
      seriesStart: decimalU64(series.start),
      seriesEnd: series.end === "" ? "" : decimalU64(series.end),
      charts,
    }),
  batchExport: (sources: string[], outputRoot: string, metricsOnly: boolean, series: SeriesRange = ALL_SERIES) =>
    command("review_batch_export", {
      sources,
      outputRoot,
      metricsOnly,
      exportSeries: series.exportSeries,
      seriesStart: decimalU64(series.start),
      seriesEnd: series.end === "" ? "" : decimalU64(series.end),
    }),
  regenerateMasks: (args: {
    source: number;
    sourcePath?: string;
    startIndex?: number | string;
    count?: number | string;
    outputPath: string;
    useRecordedConfig?: boolean;
    synthesizeBackground?: boolean;
  }) =>
    command("review_regenerate_masks", {
      source: args.source,
      sourcePath: args.sourcePath ?? "",
      startIndex: decimalU64(args.startIndex ?? 0),
      count: decimalU64(args.count ?? 0),
      outputPath: args.outputPath,
      useRecordedConfig: args.useRecordedConfig ?? true,
      synthesizeBackground: args.synthesizeBackground ?? false,
    }),
  computeCore: (coreFraction: number) => command("review_compute_core", { coreFraction }),
  computedCoreJson: () => invoke<string>("fetch_review_computed_core_json"),
  requestDensity: (bandwidthFactor = 1.0, coreFraction = 0.9, levels = 8, wantCoreRecord = true) =>
    command("review_request_density", { bandwidthFactor, coreFraction, levels, wantCoreRecord }),
  density: () => invoke<ReviewDensity>("fetch_review_density"),
  jobsBusy: () => invoke<boolean>("review_jobs_busy"),
  pollEvents: async () => (await invoke<{ events: ReviewEvent[] }>("poll_review_events")).events ?? [],
  cancel: (operationId: string) => command("cancel_review_operation", { operationId }),
};

/** Mono8 or RGB8 packet → ImageData (RGB8 = three bytes per pixel). */
export function packetToImageData(p: FramePacket): ImageData {
  const rgba = new Uint8ClampedArray(p.width * p.height * 4);
  const rgb = p.pixel_format === REVIEW_PIXEL_FORMATS.Rgb8;
  const stride = p.stride_bytes > 0 ? p.stride_bytes : p.width * (rgb ? 3 : 1);
  for (let y = 0; y < p.height; y++) {
    const src = y * stride;
    for (let x = 0; x < p.width; x++) {
      const d = (y * p.width + x) * 4;
      if (rgb) {
        const s = src + x * 3;
        rgba[d] = p.data[s] ?? 0;
        rgba[d + 1] = p.data[s + 1] ?? 0;
        rgba[d + 2] = p.data[s + 2] ?? 0;
      } else {
        const g = p.data[src + x] ?? 0;
        rgba[d] = g;
        rgba[d + 1] = g;
        rgba[d + 2] = g;
      }
      rgba[d + 3] = 255;
    }
  }
  return new ImageData(rgba, p.width, p.height);
}
