// Typed client for the review bridge commands (src-tauri/src/review.rs;
// ADR 0014, plan 2026-10-01-standalone-review-app). YOFO Review calls these
// and nothing else for review; MIB Studio's Review tab still uses its own
// commands (`bridge.ts`) until #512 step B5. Images arrive as binary frame
// packets (Mono8 or RGB8 when the backend composed an overlay / ROI);
// everything else is lossless JSON (u64 as decimal strings). Every call goes
// through `../transport` (#512 S9): Tauri IPC in the desktop shells, the
// WebSocket in a browser (where the server does not serve these commands yet,
// S11), with the same command names and arguments.
import { invoke, upload, type TransportUnsupportedError } from "../transport";
import { REVIEW_FRAME_PACKET, REVIEW_PIXEL_FORMATS } from "./reviewContract";
import { decimalU64, type FramePacket } from "../framePacket";
import { decodeReviewPacket } from "./reviewPacket";

export interface ReviewDatasetInfo {
  present: boolean;
  count: string;
  height: number;
  width: number;
  channels: number;
}

/** The planned annotation (`/experiment_annotation`, review ABI 2): `null`
 * members were not declared; `free` is sorted by key. */
export interface ReviewRunAnnotation {
  schema_version: number | null;
  sample: string | null;
  condition: string | null;
  experimental_unit: string | null;
  concentration: { value: number; unit: string } | null;
  channel: string | null;
  medium: string | null;
  temperature_c: number | null;
  notes: string | null;
  free: { key: string; value: string }[];
}

/** A file's run identity (run snapshot v3, #712; review ABI 2, #717 B1a).
 * A member the file does not record is `null`, never 0 or "": a legacy file
 * has `has_snapshot` false and every member `null`. `*_unreadable`: present
 * in the file but not a JSON object (not the same as absent). */
export interface ReviewRunIdentity {
  has_snapshot: boolean;
  snapshot_unreadable: boolean;
  snapshot_version: number | null;
  run_id: string | null;
  experiment_id: string | null;
  run_index: number | null;
  sample_ref: string | null;
  calibration_record_id: string | null;
  /** The operator selected at Start (not an authenticated identity). */
  operator: { id: string | null; name: string | null } | null;
  annotation: ReviewRunAnnotation | null;
  annotation_unreadable: boolean;
  /** Review ABI 3 (#717 B3b): snapshot v3 `emodulus_lut`; `null` when the
   * snapshot records none. */
  emodulus_lut: ReviewEModulusLut | null;
  /** The snapshot's `profile_id`, the only "method" a file records. */
  profile_id: string | null;
}

/** The E-modulus LUT a run used (snapshot v3). `loaded` false: the run had
 * no LUT, so its E values were unavailable. `null` members were not recorded. */
export interface ReviewEModulusLut {
  loaded: boolean;
  lut_id: string | null;
  revision: string | null;
  source: string | null;
  sha256: string | null;
  catalog_sha256: string | null;
  checksum_status: string | null;
}

/** One file of `review_probe_runs`: header attributes only. `ok` false =
 * `message` says why the file could not be read; nothing else is set. */
export interface ReviewRunProbe {
  path: string;
  ok: boolean;
  message: string;
  recording_file: boolean;
  identity: ReviewRunIdentity;
  /** Decimal u64 strings; `null` when the file records none. */
  start_time_ns: string | null;
  end_time_ns: string | null;
  total_valid: string | null;
  total_invalid: string | null;
  /** Contract run_completion_states; `null` for a file without run accounting. */
  accounting_completion: number | null;
  core_version: string | null;
}

/** `ok` false: the whole probe was refused (`message`): more than
 * REVIEW_PROBE.max_paths paths, or a review job is running. */
export interface ReviewProbeResult {
  ok: boolean;
  message: string;
  runs: ReviewRunProbe[];
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
  /** Review ABI 3 (#756 review 2): the stored bounding boxes' and centroids'
   * coordinate space; what decided it ("" when unknown); and the offset that
   * places a stored box in image coordinates (the ROI origin for
   * `roi_local`, else 0). Never place a box when it is `unknown`. */
  object_coordinates: "roi_local" | "image" | "unknown";
  object_coordinates_source: "attribute" | "snapshot" | "mask_check" | "roi_origin" | "";
  object_origin_x: number;
  object_origin_y: number;
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
  has_recorded_config: boolean;
  ring_ratio_min: number;
  ring_ratio_max: number;
  /** Review ABI 2: the open file's run identity (absent members `null`). */
  identity: ReviewRunIdentity;
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

/** Review ABI 3 (#717 B3b): the records of a set sharing a track id (the
 * linked records of one cell), at most REVIEW_TRACK_ROWS.max_rows. `positions`
 * are decimal-string set indices parallel to `rows`; `ok` false = refused. */
export interface ReviewTrackRows {
  ok: boolean;
  message: string;
  total: string;
  truncated: boolean;
  positions: string[];
  rows: ReviewRow[];
}

/** Review ABI 3 (#717 B3b): one record reprocessed next to its stored row;
 * never written. `settings` is a REVIEW_REPROCESS.settings value;
 * `match_index` points into `reprocessed` (`null`: no object matches).
 * `ok` false = refused (`message`), `stored` null. */
export interface ReviewReprocess {
  ok: boolean;
  message: string;
  settings: number;
  used_recorded_settings: boolean;
  settings_label: string;
  core_version: string;
  core_source: string;
  /** The px→µm factor the reprocessing ran and reports with (named in settings_label). */
  pixel_to_micron: number;
  /** False without an E-LUT: the reprocessed rows' E is then null (not recomputed). */
  emodulus_recomputed: boolean;
  stored: ReviewRow | null;
  reprocessed: ReviewRow[];
  match_index: number | null;
  object_count_differs: boolean;
  truncated: boolean;
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
  ring_ratio: number[];
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

export const PULL_REVIEW = REVIEW_FRAME_PACKET.pull_kinds.review;
export const PULL_REVIEW_THUMBNAILS = REVIEW_FRAME_PACKET.pull_kinds.review_thumbnails;
export const PULL_REVIEW_SERIES = REVIEW_FRAME_PACKET.pull_kinds.review_series;

// Local generation guard: a reply that started before a file change is
// retired by the FramePullScheduler's epochs; this only rejects replies that
// straddle an open/close within one pull.
let generation = 0;

async function pull(command: string, kind: number, args: Record<string, unknown>): Promise<FramePacket> {
  const g = generation;
  const buffer = await invoke<ArrayBuffer>(command, args);
  if (g !== generation) throw new Error("FRAME_REPLY_STALE");
  return decodeReviewPacket(buffer, kind);
}

async function command(name: string, args?: Record<string, unknown>): Promise<ReviewCmdResult> {
  const r = await invoke<ReviewCmdResult>(name, args);
  return { ok: !!r.ok, command: Number(r.command), message: String(r.message ?? ""), operation_id: decimalU64(r.operation_id ?? "0") };
}

export const reviewBridge = {
  abiVersion: () => invoke<number>("review_abi_version"),
  /** Isoelastic reference curves embedded in the app (descending E). */
  isoelasticCurves: () =>
    invoke<{ source: string; curves: { emodulus_kpa: number; points: [number, number][] }[] }>("fetch_isoelastic_curves"),
  /** File passed on the command line / by a file association ("" = none). */
  launchPath: () => invoke<string>("review_launch_path"),
  /** A pending OS open request (macOS Finder) taken once, "" when none. */
  takeOpenRequest: () => invoke<string>("review_take_open_request"),
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
  /** Review ABI 2 (#717 B1a): the identity probe for the experiment library,
   * header-only, at most REVIEW_PROBE.max_paths paths per call, one result
   * per path in order (per-file errors). Refused as a whole while a review
   * job runs. The open file is left as it was. */
  probeRuns: async (paths: string[]): Promise<ReviewProbeResult> => {
    const r = await invoke<ReviewProbeResult>("review_probe_runs", { paths });
    return { ok: !!r.ok, message: String(r.message ?? ""), runs: r.runs ?? [] };
  },
  rows: (valid: boolean, offset: number | string, count: number) =>
    invoke<ReviewRows>("fetch_review_rows", { valid, offset: decimalU64(offset), count }),
  /** Review ABI 3 (#717 B3b): the linked records of one cell (its track),
   * from the metadata read at open. */
  trackRows: async (valid: boolean, trackId: number, max: number): Promise<ReviewTrackRows> => {
    const r = await invoke<ReviewTrackRows>("fetch_review_track_rows", { valid, trackId, max });
    return {
      ok: !!r.ok,
      message: String(r.message ?? ""),
      total: decimalU64(r.total ?? "0"),
      truncated: !!r.truncated,
      positions: (r.positions ?? []).map((p) => decimalU64(p)),
      rows: r.rows ?? [],
    };
  },
  /** Review ABI 3 (#717 B3b): reprocess one record with the review kernel and
   * return it next to the stored row. Never writes; refused while a review
   * job runs. */
  reprocessRecord: async (valid: boolean, index: number | string, settings: number): Promise<ReviewReprocess> => {
    const r = await invoke<ReviewReprocess>("review_reprocess_record", { valid, index: decimalU64(index), settings });
    return {
      ...r,
      ok: !!r.ok,
      message: String(r.message ?? ""),
      stored: r.ok ? (r.stored ?? null) : null,
      reprocessed: r.reprocessed ?? [],
      match_index: r.match_index ?? null,
    };
  },
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
  /** Export All; chart snapshots staged beforehand (charts/chartExport.ts
   * `stageCharts`) are consumed by this call. */
  exportAll: (outputRoot: string, series: SeriesRange = ALL_SERIES) =>
    command("review_export_all", {
      outputRoot,
      exportSeries: series.exportSeries,
      seriesStart: decimalU64(series.start),
      seriesEnd: series.end === "" ? "" : decimalU64(series.end),
    }),
  /** Export Charts: the staged snapshots as files in `outputDir`. */
  exportCharts: (outputDir: string) => command("review_export_charts", { outputDir }),
  /** Drop every staged chart snapshot. */
  clearCharts: () => invoke<void>("review_clear_charts"),
  /** Stage one chart snapshot: the PNG bytes as the raw request body, the
   * file name in header `x-chart-name`; resolves with how many are staged.
   * Rejects with TransportUnsupportedError over the WebSocket. */
  stageChart: (name: string, bytes: Uint8Array) => upload<number>("review_stage_chart", bytes, { "x-chart-name": name }),
  /** File names in `dir` (default export names, exports/naming.ts). */
  listDir: (dir: string) => invoke<string[]>("review_list_dir", { dir }),
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

/** Plain words for a request the transport cannot carry (YOFO Studio in a
 * browser: the WebSocket server reads no raw request bodies, so chart
 * snapshots cannot be staged there until #512 S11). */
export function unsupportedText(e: TransportUnsupportedError): string {
  return e.operation === "review_stage_chart"
    ? "Chart images cannot be sent to the instrument over this browser connection yet, so exports that include charts cannot run here."
    : `This browser connection cannot do that yet (${e.operation}).`;
}

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
