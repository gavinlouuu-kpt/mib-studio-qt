// The live Monitoring density (KDE), its reference contour, the E-modulus table and the stats telemetry
// (bridge ABI next (#886, set at merge), #866 PR E; bridge-contract.json `monitoring_density`, `kde_reference_from_file`,
// `emodulus_lut`, `processing_stats`).
//
// The backend owns all of it: MonitoringDensityService computes the estimate on its own lowest-priority
// worker, the settings are pushed to it and its last result is polled by generation. This module only
// types and checks what the commands return, and builds their arguments. A reply that breaks the
// contract throws MONITORING_DENSITY_INVALID before the shell draws anything from it.
import { EMODULUS_LUT, KDE_REFERENCE_FROM_FILE, MONITORING_DENSITY, PROCESSING_STATS } from "./bridgeContract";
import { decimalU64 } from "./framePacket";
import { cellKeyString, type MonitoringCellKeyInput } from "./monitoringCells";

function invalid(why: string): never {
  throw new Error(`MONITORING_DENSITY_INVALID: ${why}`);
}

const isObject = (v: unknown): v is Record<string, unknown> => typeof v === "object" && v !== null && !Array.isArray(v);
const finite = (v: unknown, key: string): number => {
  if (typeof v !== "number" || !Number.isFinite(v)) invalid(`${key} is not a finite number`);
  return v;
};
const finiteOrNull = (v: unknown, key: string): number | null => (v === null ? null : finite(v, key));
const integer = (v: unknown, key: string): number => {
  if (typeof v !== "number" || !Number.isInteger(v)) invalid(`${key} is not an integer`);
  return v;
};
const boolean = (v: unknown, key: string): boolean => {
  if (typeof v !== "boolean") invalid(`${key} is not a boolean`);
  return v;
};
const text = (v: unknown, key: string): string => {
  if (typeof v !== "string") invalid(`${key} is not a string`);
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

// ---- settings ---------------------------------------------------------------------------------

/** The density settings as the service applies them, after its clamping (bridge-contract.json
 *  `monitoring_density`: interval 500..60000 ms, bandwidth factor 0.2..5, core fraction 0.05..1).
 *  x0, x1, y0, y1 are the scatter's fixed axes (um^2, deformability) the contour grid spans; all
 *  zero (or x1 <= x0, y1 <= y0) means the data range padded by 10%. */
export interface MonitoringDensitySettings {
  enabled: boolean;
  interval_ms: number;
  bandwidth_factor: number;
  core_fraction: number;
  x0: number;
  x1: number;
  y0: number;
  y1: number;
}

/** What a shell pushes. Omitted members take the backend defaults (2000 ms, 1, 0.9, axes 0). The
 *  shell sends `enabled: false` when the Monitoring sub-tab is hidden, as Qt does. */
export interface MonitoringDensitySettingsInput {
  enabled: boolean;
  intervalMs?: number;
  bandwidthFactor?: number;
  coreFraction?: number;
  x0?: number;
  x1?: number;
  y0?: number;
  y1?: number;
}

/** The `monitoring_density_set_settings` arguments; throws INVALID_ARGUMENTS before anything is sent
 *  for a member that is not a finite number (the interval must be an integer). The backend clamps
 *  the ranges; the reply says what applies. */
export function monitoringDensitySettingsArgs(input: MonitoringDensitySettingsInput): Record<string, number | boolean> {
  if (typeof input.enabled !== "boolean") throw new Error("INVALID_ARGUMENTS: enabled must be a boolean");
  const args: Record<string, number | boolean> = { enabled: input.enabled };
  for (const key of ["bandwidthFactor", "coreFraction", "x0", "x1", "y0", "y1"] as const) {
    const v = input[key];
    if (v === undefined) continue;
    if (typeof v !== "number" || !Number.isFinite(v)) throw new Error(`INVALID_ARGUMENTS: ${key} must be a finite number`);
    args[key] = v;
  }
  if (input.intervalMs !== undefined) {
    if (!Number.isInteger(input.intervalMs)) throw new Error("INVALID_ARGUMENTS: intervalMs must be an integer");
    args.intervalMs = input.intervalMs;
  }
  return args;
}

function decodeSettings(raw: unknown, what: string): MonitoringDensitySettings {
  if (!isObject(raw)) return invalid(`${what} is not an object`);
  return {
    enabled: boolean(raw.enabled, `${what}.enabled`),
    interval_ms: integer(raw.interval_ms, `${what}.interval_ms`),
    bandwidth_factor: finite(raw.bandwidth_factor, `${what}.bandwidth_factor`),
    core_fraction: finite(raw.core_fraction, `${what}.core_fraction`),
    x0: finite(raw.x0, `${what}.x0`),
    x1: finite(raw.x1, `${what}.x1`),
    y0: finite(raw.y0, `${what}.y0`),
    y1: finite(raw.y1, `${what}.y1`),
  };
}

export interface MonitoringDensitySettingsReply {
  ok: boolean;
  message: string;
  /** What the service applies; null when the backend is not initialized. */
  settings: MonitoringDensitySettings | null;
}

export function decodeMonitoringDensitySettingsReply(raw: unknown): MonitoringDensitySettingsReply {
  if (!isObject(raw)) return invalid("settings reply is not an object");
  const ok = boolean(raw.ok, "ok");
  const settings = raw.settings === null ? null : decodeSettings(raw.settings, "settings");
  if (ok && settings === null) invalid("an accepted settings reply carries the applied settings");
  if (settings) {
    // The service clamps: a reply outside the bounds is a contract breach.
    if (settings.interval_ms < MONITORING_DENSITY.interval_ms_min || settings.interval_ms > MONITORING_DENSITY.interval_ms_max)
      invalid("settings.interval_ms outside its bounds");
    if (settings.bandwidth_factor < MONITORING_DENSITY.bandwidth_factor_min || settings.bandwidth_factor > MONITORING_DENSITY.bandwidth_factor_max)
      invalid("settings.bandwidth_factor outside its bounds");
    if (settings.core_fraction < MONITORING_DENSITY.core_fraction_min || settings.core_fraction > MONITORING_DENSITY.core_fraction_max)
      invalid("settings.core_fraction outside its bounds");
  }
  return { ok, message: text(raw.message, "message"), settings };
}

// ---- the density result ----------------------------------------------------------------------

/** The points of one estimate, oldest -> newest, as parallel arrays (the payload stays small: the run
 *  and the capture are listed once in `contexts`). A point names a monitoring row by
 *  (start_generation, capture_epoch, frame_index, object_id): match a snapshot row with
 *  `densityByRow`. */
export interface MonitoringDensityPoints {
  count: number;
  /** The distinct (run, capture) pairs, in order of first appearance. */
  contexts: { start_generation: string; capture_epoch: string }[];
  /** Per point: its index into `contexts`. */
  context: number[];
  frame_index: string[];
  object_id: number[];
  /** Per point: the density normalised to [0, 1] (the estimate's maximum is 1); null if not finite. */
  density: (number | null)[];
}

export interface MonitoringDensityResult {
  points: MonitoringDensityPoints;
  /** Silverman bandwidth times the factor, per axis (um^2, deformability). */
  bandwidth: { x: number | null; y: number | null };
  bandwidth_factor: number;
  core_fraction: number;
  /** null below three cells: no core, no contour (Qt: "too few cells for a contour"). */
  core_level: number | null;
  core_count: number;
  /** Closed loops in um^2 x deformability, the last vertex repeating the first. */
  contours: [number, number][][];
  pixel_to_micron: number | null;
  /** The range the contour was traced on. */
  grid: { x0: number; x1: number; y0: number; y1: number; nx: number; ny: number };
  compute_ms: number;
  compute_us: number;
  wall_ms: number;
  computed_at_ns: string;
}

export interface MonitoringDensityStats {
  next_interval_ms: number;
  /** What the Qt tooltip calls "refreshed every": max(settings.interval_ms, next_interval_ms). */
  refresh_ms: number;
  estimates: string;
  skipped_unchanged: string;
  skipped_under_load: string;
  /** A requested tick has not finished yet. */
  busy: boolean;
  priority_lowered: boolean;
}

export type MonitoringDensityReply =
  | { valid: false }
  /** Nothing newer than the generation asked about: change nothing. */
  | { valid: true; unchanged: true; generation: string }
  /** A newer generation. `result` is null when the service holds no estimate (disabled, or the ring
   *  emptied): clear the densities and the contours. */
  | {
      valid: true;
      unchanged: false;
      generation: string;
      settings: MonitoringDensitySettings;
      stats: MonitoringDensityStats;
      result: MonitoringDensityResult | null;
    };

function decodePoints(raw: unknown): MonitoringDensityPoints {
  if (!isObject(raw)) return invalid("points is not an object");
  const count = integer(raw.count, "points.count");
  if (count < 0 || count > MONITORING_DENSITY.max_points) invalid("points.count outside 0..max_points");
  const arrays = {
    contexts: raw.contexts,
    context: raw.context,
    frame_index: raw.frame_index,
    object_id: raw.object_id,
    density: raw.density,
  };
  for (const [key, value] of Object.entries(arrays)) if (!Array.isArray(value)) invalid(`points.${key} is not an array`);
  const contextsRaw = raw.contexts as unknown[];
  const context = raw.context as unknown[];
  const frames = raw.frame_index as unknown[];
  const objects = raw.object_id as unknown[];
  const density = raw.density as unknown[];
  if (context.length !== count || frames.length !== count || objects.length !== count || density.length !== count)
    invalid("the point arrays are not parallel");
  if (contextsRaw.length > count) invalid("more contexts than points");
  const contexts = contextsRaw.map((c, i) => {
    if (!isObject(c)) return invalid(`points.contexts[${i}] is not an object`);
    return {
      start_generation: decimal(c.start_generation, `points.contexts[${i}].start_generation`),
      capture_epoch: decimal(c.capture_epoch, `points.contexts[${i}].capture_epoch`),
    };
  });
  return {
    count,
    contexts,
    context: context.map((v, i) => {
      const at = integer(v, `points.context[${i}]`);
      if (at < 0 || at >= contexts.length) invalid(`points.context[${i}] names no context`);
      return at;
    }),
    frame_index: frames.map((v, i) => decimal(v, `points.frame_index[${i}]`)),
    object_id: objects.map((v, i) => integer(v, `points.object_id[${i}]`)),
    density: density.map((v, i) => {
      const d = finiteOrNull(v, `points.density[${i}]`);
      if (d !== null && (d < 0 || d > 1)) invalid(`points.density[${i}] is outside [0, 1]`);
      return d;
    }),
  };
}

function decodeContours(raw: unknown): [number, number][][] {
  if (!Array.isArray(raw)) return invalid("contours is not an array");
  return raw.map((loop, i) => {
    if (!Array.isArray(loop)) return invalid(`contours[${i}] is not an array`);
    return loop.map((p, j) => {
      if (!Array.isArray(p) || p.length !== 2) return invalid(`contours[${i}][${j}] is not [x, y]`);
      return [finite(p[0], `contours[${i}][${j}][0]`), finite(p[1], `contours[${i}][${j}][1]`)] as [number, number];
    });
  });
}

function decodeResult(raw: unknown): MonitoringDensityResult {
  if (!isObject(raw)) return invalid("result is not an object");
  if (!isObject(raw.bandwidth) || !isObject(raw.grid)) return invalid("bandwidth / grid are not objects");
  const coreLevel = finiteOrNull(raw.core_level, "core_level");
  const contours = decodeContours(raw.contours);
  if (coreLevel === null && contours.length > 0) invalid("a contour without a core level");
  const g = raw.grid;
  return {
    points: decodePoints(raw.points),
    bandwidth: { x: finiteOrNull(raw.bandwidth.x, "bandwidth.x"), y: finiteOrNull(raw.bandwidth.y, "bandwidth.y") },
    bandwidth_factor: finite(raw.bandwidth_factor, "bandwidth_factor"),
    core_fraction: finite(raw.core_fraction, "core_fraction"),
    core_level: coreLevel,
    core_count: integer(raw.core_count, "core_count"),
    contours,
    pixel_to_micron: finiteOrNull(raw.pixel_to_micron, "pixel_to_micron"),
    grid: {
      x0: finite(g.x0, "grid.x0"), x1: finite(g.x1, "grid.x1"), y0: finite(g.y0, "grid.y0"), y1: finite(g.y1, "grid.y1"),
      nx: integer(g.nx, "grid.nx"), ny: integer(g.ny, "grid.ny"),
    },
    compute_ms: integer(raw.compute_ms, "compute_ms"),
    compute_us: integer(raw.compute_us, "compute_us"),
    wall_ms: integer(raw.wall_ms, "wall_ms"),
    computed_at_ns: decimal(raw.computed_at_ns, "computed_at_ns"),
  };
}

function decodeStats(raw: unknown): MonitoringDensityStats {
  if (!isObject(raw)) return invalid("stats is not an object");
  return {
    next_interval_ms: integer(raw.next_interval_ms, "stats.next_interval_ms"),
    refresh_ms: integer(raw.refresh_ms, "stats.refresh_ms"),
    estimates: decimal(raw.estimates, "stats.estimates"),
    skipped_unchanged: decimal(raw.skipped_unchanged, "stats.skipped_unchanged"),
    skipped_under_load: decimal(raw.skipped_under_load, "stats.skipped_under_load"),
    busy: boolean(raw.busy, "stats.busy"),
    priority_lowered: boolean(raw.priority_lowered, "stats.priority_lowered"),
  };
}

/** fetch_monitoring_density: checked against the contract; throws MONITORING_DENSITY_INVALID. */
export function decodeMonitoringDensity(raw: unknown): MonitoringDensityReply {
  if (!isObject(raw)) return invalid("reply is not an object");
  if (raw.valid === false) return { valid: false };
  if (raw.valid !== true) return invalid("valid");
  const generation = decimal(raw.generation, "generation");
  if (boolean(raw.unchanged, "unchanged")) return { valid: true, unchanged: true, generation };
  return {
    valid: true,
    unchanged: false,
    generation,
    settings: decodeSettings(raw.settings, "settings"),
    stats: decodeStats(raw.stats),
    result: raw.result === null ? null : decodeResult(raw.result),
  };
}

/** The `fetch_monitoring_density` arguments: `sinceGeneration` is the generation last seen (absent
 *  asks for whatever there is). */
export function monitoringDensityArgs(sinceGeneration?: string | number | bigint | null): Record<string, string> {
  return sinceGeneration === undefined || sinceGeneration === null ? {} : { sinceGeneration: decimalU64(sinceGeneration) };
}

/** The densities of a result keyed by the row they belong to (`cellKeyString`: start generation,
 *  capture epoch, frame index, object id), for matching a monitoring snapshot row in O(1). A row the
 *  map does not hold is newer than the estimate and has no density yet (Qt's unmatched rows keep
 *  the sparsest level). Points whose density is null are left out. */
export function densityByRow(result: MonitoringDensityResult): Map<string, number> {
  const p = result.points;
  const out = new Map<string, number>();
  for (let i = 0; i < p.count; i += 1) {
    const d = p.density[i];
    if (d === null) continue;
    const ctx = p.contexts[p.context[i]];
    out.set(cellKeyString({ ...ctx, frame_index: p.frame_index[i], object_id: p.object_id[i] }), d);
  }
  return out;
}

/** The key `densityByRow` uses for a snapshot row (or any monitoring cell). */
export const densityRowKey = (key: MonitoringCellKeyInput): string => cellKeyString(key);

// ---- the reference contour from a file --------------------------------------------------------

export type KdeReferenceSource = "full-run" | "live";

export interface KdeReference {
  /** true: the contours are loaded; false: `error` says why (nothing was loaded or changed). */
  ok: boolean;
  path: string;
  file_name: string;
  /** "No reference contour loaded from <path>: <reason>" when not ok, else null. */
  error: string | null;
  reason: string | null;
  source: KdeReferenceSource | null;
  core_fraction: number | null;
  core_percent: number | null;
  loops: number;
  /** Closed loops in um^2 x deformability. */
  contours: [number, number][][];
}

/** `fetch_kde_reference_from_file`'s argument; throws INVALID_ARGUMENTS before anything is sent for a
 *  path outside the contract's bounds. */
export function kdeReferenceArgs(path: string): { path: string } {
  const bytes = new TextEncoder().encode(path).length;
  if (bytes < 1 || bytes > KDE_REFERENCE_FROM_FILE.path_max_bytes)
    throw new Error(`INVALID_ARGUMENTS: a file path of 1 to ${KDE_REFERENCE_FROM_FILE.path_max_bytes} bytes is required`);
  return { path };
}

export function decodeKdeReference(raw: unknown): KdeReference {
  if (!isObject(raw)) return invalid("reference reply is not an object");
  const ok = boolean(raw.ok, "ok");
  const contours = decodeContours(raw.contours);
  const loops = integer(raw.loops, "loops");
  if (loops !== contours.length) invalid("loops differs from the contours listed");
  if (contours.reduce((n, l) => n + l.length, 0) > KDE_REFERENCE_FROM_FILE.max_vertices) invalid("contours over max_vertices");
  const error = raw.error === null ? null : text(raw.error, "error");
  const reason = raw.reason === null ? null : text(raw.reason, "reason");
  if (ok !== (error === null)) invalid("error is set exactly when the load failed");
  if (ok === (contours.length === 0)) invalid("contours are listed exactly when the load succeeded");
  const source = raw.source;
  if (source !== null && source !== "full-run" && source !== "live") invalid("source");
  return {
    ok,
    path: text(raw.path, "path"),
    file_name: text(raw.file_name, "file_name"),
    error,
    reason,
    source,
    core_fraction: finiteOrNull(raw.core_fraction, "core_fraction"),
    core_percent: raw.core_percent === null ? null : integer(raw.core_percent, "core_percent"),
    loops,
    contours,
  };
}

/** Qt's label for a loaded reference: "<file>, <full-run | live estimate> <percent>%". */
export function kdeReferenceLabel(reference: KdeReference): string | null {
  if (!reference.ok || reference.source === null || reference.core_percent === null) return null;
  return `${reference.file_name}, ${reference.source === "live" ? "live estimate" : "full-run"} ${reference.core_percent}%`;
}

// ---- the E-modulus table -----------------------------------------------------------------------

/** The grid of the loaded E-modulus table: values[deformIndex * area_bins + areaIndex] is the Young's
 *  modulus (kPa) at area_min + areaIndex * area_step (um^2) and deform_min + deformIndex *
 *  deform_step; null where the table has no value (outside its coverage). */
export interface EmodulusGrid {
  area_min: number;
  area_step: number;
  area_bins: number;
  deform_min: number;
  deform_step: number;
  deform_bins: number;
  values: (number | null)[];
}

export interface EmodulusLutInfo {
  path: string;
  sha256: string;
  lut_id: string;
  revision: string;
  source: string;
  catalog_sha256: string;
  checksum_status: string;
}

export type EmodulusLutReply =
  | { valid: false; message?: string }
  /** No table is loaded: no E readout, no E range in the gate overlay (Qt's behaviour without a table). */
  | { valid: true; loaded: false }
  /** The table the caller already holds (identity equal to the one it sent). */
  | { valid: true; loaded: true; unchanged: true; identity: string; info: EmodulusLutInfo }
  | { valid: true; loaded: true; unchanged: false; identity: string; info: EmodulusLutInfo; lut: EmodulusGrid };

/** `fetch_emodulus_lut`'s arguments: send the identity of the table you hold and the grid comes back
 *  only when it differs. */
export function emodulusLutArgs(knownIdentity?: string | null): { knownIdentity?: string } {
  if (knownIdentity === undefined || knownIdentity === null || knownIdentity === "") return {};
  if (new TextEncoder().encode(knownIdentity).length > EMODULUS_LUT.known_identity_max_bytes)
    throw new Error(`INVALID_ARGUMENTS: knownIdentity is longer than ${EMODULUS_LUT.known_identity_max_bytes} bytes`);
  return { knownIdentity };
}

function decodeLutInfo(raw: unknown): EmodulusLutInfo {
  if (!isObject(raw)) return invalid("info is not an object");
  return {
    path: text(raw.path, "info.path"),
    sha256: text(raw.sha256, "info.sha256"),
    lut_id: text(raw.lut_id, "info.lut_id"),
    revision: text(raw.revision, "info.revision"),
    source: text(raw.source, "info.source"),
    catalog_sha256: text(raw.catalog_sha256, "info.catalog_sha256"),
    checksum_status: text(raw.checksum_status, "info.checksum_status"),
  };
}

function decodeGrid(raw: unknown): EmodulusGrid {
  if (!isObject(raw)) return invalid("lut is not an object");
  const areaBins = integer(raw.area_bins, "lut.area_bins");
  const deformBins = integer(raw.deform_bins, "lut.deform_bins");
  if (areaBins < 2 || deformBins < 2) invalid("a table needs at least two bins per axis");
  const areaStep = finite(raw.area_step, "lut.area_step");
  const deformStep = finite(raw.deform_step, "lut.deform_step");
  if (!(areaStep > 0) || !(deformStep > 0)) invalid("a table's steps are positive");
  if (!Array.isArray(raw.values) || raw.values.length !== areaBins * deformBins) invalid("lut.values is not area_bins x deform_bins");
  return {
    area_min: finite(raw.area_min, "lut.area_min"),
    area_step: areaStep,
    area_bins: areaBins,
    deform_min: finite(raw.deform_min, "lut.deform_min"),
    deform_step: deformStep,
    deform_bins: deformBins,
    values: raw.values.map((v, i) => finiteOrNull(v, `lut.values[${i}]`)),
  };
}

export function decodeEmodulusLut(raw: unknown): EmodulusLutReply {
  if (!isObject(raw)) return invalid("reply is not an object");
  if (raw.valid === false) return typeof raw.message === "string" ? { valid: false, message: raw.message } : { valid: false };
  if (raw.valid !== true) return invalid("valid");
  if (!boolean(raw.loaded, "loaded")) return { valid: true, loaded: false };
  const identity = text(raw.identity, "identity");
  if (identity === "") invalid("a loaded table has an identity");
  const info = decodeLutInfo(raw.info);
  if (boolean(raw.unchanged, "unchanged")) {
    if (raw.lut !== null) invalid("an unchanged table carries no grid");
    return { valid: true, loaded: true, unchanged: true, identity, info };
  }
  return { valid: true, loaded: true, unchanged: false, identity, info, lut: decodeGrid(raw.lut) };
}

// ---- processing and camera telemetry (fetch_processing_stats additions) --------------------------

export type MetricState = (typeof PROCESSING_STATS.metric_states)[number];
export type RealtimeMode = (typeof PROCESSING_STATS.realtime_modes)[number];

/** One acquisition telemetry sample: the device or SDK's own report with its validity. `value` and
 *  `age_ms` are null unless the sample is valid or stale; an unknown rate is never a 0. Qt shows
 *  "n/a" for unavailable, "unsupported", "error", "<value> (stale <age> s)" for stale. */
export interface MetricSample {
  state: MetricState;
  value: number | null;
  age_ms: number | null;
}

export interface CameraTelemetry {
  running: boolean;
  /** Frames per second. */
  frame_rate: MetricSample;
  /** Megabytes per second. */
  data_rate_mbps: MetricSample;
}

/** The ABI next (#886, set at merge) additions to the processing stats that this module owns: the realtime mode and the
 *  camera telemetry. `algo_avg_us` and `algo_avg_age_ms`, also new in ABI next (#886, set at merge), are decoded by PR #877's
 *  `decodeProcessingStats` under those names (not here, to leave that decoder to that PR). */
export interface ProcessingTelemetry {
  /** null when the backend is not initialized or too old to report it. */
  realtime_mode: RealtimeMode | null;
  /** null when the backend is not initialized or too old to report it. */
  camera: CameraTelemetry | null;
}

const METRIC_STATES = new Set<string>(PROCESSING_STATS.metric_states);
const REALTIME_MODES = new Set<string>(PROCESSING_STATS.realtime_modes);

function decodeMetricSample(raw: unknown, key: string): MetricSample {
  if (!isObject(raw)) return invalid(`${key} is not an object`);
  if (typeof raw.state !== "string" || !METRIC_STATES.has(raw.state)) return invalid(`${key}.state is not a contract metric state`);
  const value = finiteOrNull(raw.value, `${key}.value`);
  const age = finiteOrNull(raw.age_ms, `${key}.age_ms`);
  const hasValue = raw.state === "valid" || raw.state === "stale";
  if (hasValue !== (value !== null)) invalid(`${key}.value is set exactly for a valid or stale sample`);
  return { state: raw.state as MetricState, value, age_ms: age };
}

/** The telemetry members of a `fetch_processing_stats` reply; members an older backend does not send
 *  decode as null (the shell hides the row). */
export function decodeProcessingTelemetry(raw: unknown): ProcessingTelemetry {
  if (!isObject(raw)) return invalid("processing stats is not an object");
  const mode = raw.realtime_mode;
  if (mode !== undefined && mode !== null && (typeof mode !== "string" || !REALTIME_MODES.has(mode))) invalid("realtime_mode");
  let camera: CameraTelemetry | null = null;
  if (raw.camera !== undefined && raw.camera !== null) {
    if (!isObject(raw.camera)) invalid("camera is not an object");
    camera = {
      running: boolean(raw.camera.running, "camera.running"),
      frame_rate: decodeMetricSample(raw.camera.frame_rate, "camera.frame_rate"),
      data_rate_mbps: decodeMetricSample(raw.camera.data_rate_mbps, "camera.data_rate_mbps"),
    };
  }
  return {
    realtime_mode: typeof mode === "string" ? (mode as RealtimeMode) : null,
    camera,
  };
}

/** Qt's text for a telemetry sample (StatsDisplayManager::formatMetric): the value as a whole number,
 *  "<value> (stale <seconds> s)" for a stale one, "n/a" for an unavailable one, then "unsupported" and
 *  "error". */
export function formatMetricSample(sample: MetricSample): string {
  switch (sample.state) {
    case "valid":
      return sample.value === null ? "n/a" : String(Math.trunc(sample.value));
    case "stale":
      return `${sample.value === null ? "n/a" : Math.trunc(sample.value)} (stale ${((sample.age_ms ?? 0) / 1000).toFixed(1)} s)`;
    case "unavailable":
      return "n/a";
    case "unsupported":
      return "unsupported";
    case "error":
      return "error";
  }
}
