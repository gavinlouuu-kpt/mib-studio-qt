// Run quick look (bridge ABI 44 `run_quick_look`, review ABI 6 `review_run_statistics`; #716 A7a,
// bridge-contract.json `run_quick_look`).
//
// The per-run descriptive statistics of closed HDF5 experiment runs, computed by one C++ function
// for both commands (at close into an in-memory cache, or on demand while the instrument is idle;
// `deferred` otherwise). This module checks the reply's shape and builds the request; it computes
// nothing: every number and every delta is the backend's. Counters stay decimal strings and an
// unrecorded value stays null (never 0). No UI here: A7b's Summary and Quick look consume it.
import { RUN_QUICK_LOOK } from "./bridgeContract";
import { REVIEW_EMODULUS_LUT_PROVENANCES } from "./review/reviewContract";

export type RunQuickLookStatus = "computed" | "deferred" | "not_available" | "error";
const STATUSES: readonly RunQuickLookStatus[] = ["computed", "deferred", "not_available", "error"];
export type RunEmodulusStatus = "computed" | "no_lut" | "not_recorded";
/** Where the E-LUT record comes from: the identity's emodulus_lut_provenance (review ABI 5, #783). */
export type RunEmodulusLutProvenance = (typeof REVIEW_EMODULUS_LUT_PROVENANCES)[number];
const E_STATUSES: readonly RunEmodulusStatus[] = ["computed", "no_lut", "not_recorded"];
export type RunEventRateStatus = "computed" | "not_recorded" | "too_short";
const RATE_STATUSES: readonly RunEventRateStatus[] = ["computed", "not_recorded", "too_short"];
export type RunQuickLookFlag =
  | "low_n" | "loss" | "legacy" | "no_accounting" | "e_out_of_domain" | "factor_not_recorded" | "run_not_finalized";
const FLAGS: readonly RunQuickLookFlag[] =
  ["low_n", "loss", "legacy", "no_accounting", "e_out_of_domain", "factor_not_recorded", "run_not_finalized"];
export type RunDeltaWithheld = "factor_not_recorded" | "no_emodulus" | "lut_unidentified" | "lut_differs" | "no_value";
const WITHHELD: readonly RunDeltaWithheld[] = ["factor_not_recorded", "no_emodulus", "lut_unidentified", "lut_differs", "no_value"];
export type RunIdentityComparison = "same" | "differs" | "unknown";
const COMPARISONS: readonly RunIdentityComparison[] = ["same", "differs", "unknown"];

/** A decimal string counter (u64 on the wire). */
export type Decimal = string;

export interface RunQuickLookReference {
  path: string;
  /** Required: null only for a run whose file records no run_id. The backend confirms it. */
  run_id: string | null;
  /** Client-held with the plan; echoed, not confirmed. */
  start_generation?: Decimal | null;
}

export interface RunStatistics {
  schema_version: number;
  identity: {
    has_snapshot: boolean;
    snapshot_unreadable: boolean;
    snapshot_version: number | null;
    run_id: string | null;
    experiment_id: string | null;
    run_index: number | null;
    sample_ref: string | null;
    calibration_record_id: string | null;
    operator: { id: string | null; name: string | null } | null;
    annotation: (Record<string, unknown> & { condition: string | null; sample: string | null }) | null;
    annotation_unreadable: boolean;
    profile_id: string | null;
    method_revision_id: string | null;
    config_canonical_sha256: string | null;
    core_version: string | null;
    contract_version: number | null;
    start_generation: Decimal | null;
    start_time_ns: Decimal | null;
    end_time_ns: Decimal | null;
  };
  counts: { set_rows: Decimal; valid: Decimal; finite: Decimal; excluded: Decimal };
  accounting: (Record<string, unknown> & { completion: string; admitted: Decimal }) | null;
  /** The file's recorded factor only: all null when it records none. */
  area_um2: { n: Decimal | null; median: number | null; pixel_to_micron: number | null };
  deformability: { n: Decimal; median: number | null };
  emodulus: {
    status: RunEmodulusStatus;
    n: Decimal | null;
    median_kpa: number | null;
    out_of_domain: Decimal | null;
    lut: { loaded: boolean; lut_id: string | null; revision: string | null; source: string | null;
      sha256: string | null; provenance: RunEmodulusLutProvenance } | null;
  };
  /** "% in the run's declared target group" (no polygon). */
  target_group: { declared: boolean | null; n: Decimal | null; of: Decimal | null; percent: number | null;
    bounds: Record<string, number | boolean | null> | null };
  event_rate: { status: RunEventRateStatus; per_s: number | null; span_s: number | null; ticks_per_second: Decimal | null };
  flags: RunQuickLookFlag[];
  /** Display only, outside the parity claim. */
  overlay: { stride: Decimal; points: Decimal; area_px: number[]; area_um2: number[] | null; deformability: number[];
    target_group: (0 | 1)[] };
}

export interface RunDeltaValue { value: number | null; withheld: RunDeltaWithheld | null }
export interface RunDelta {
  area_um2_median: RunDeltaValue;
  deformability_median: RunDeltaValue;
  emodulus_median_kpa: RunDeltaValue;
  differs: { profile: RunIdentityComparison; method_revision: RunIdentityComparison; core: RunIdentityComparison;
    contract: RunIdentityComparison };
}

export interface RunQuickLookRun {
  path: string;
  status: RunQuickLookStatus;
  message: string;
  computed_at: "close" | "on_demand" | null;
  from_cache: boolean;
  file: { canonical_path: string; size_bytes: Decimal } | null;
  statistics: RunStatistics | null;
  delta: RunDelta | null;
}

export interface RunQuickLookReply {
  ok: boolean;
  message: string;
  schema_version: number;
  runs: RunQuickLookRun[];
  reference: {
    requested: { path: string; run_id: string | null; start_generation: string | null };
    confirmed: boolean;
    message: string;
    run: RunQuickLookRun | null;
  } | null;
}

const invalid = (why: string): never => {
  throw new Error(`RUN_QUICK_LOOK_REPLY_INVALID: ${why}`);
};
const isObject = (v: unknown): v is Record<string, unknown> => typeof v === "object" && v !== null && !Array.isArray(v);
const isDecimal = (v: unknown): v is string => typeof v === "string" && /^\d{1,20}$/.test(v);
const decimalOrNull = (v: unknown) => v === null || isDecimal(v);
const numberOrNull = (v: unknown) => v === null || (typeof v === "number" && Number.isFinite(v));
const utf8Bytes = (s: string) => new TextEncoder().encode(s).length;

function checkDelta(v: unknown): RunDelta | null {
  if (v === null) return null;
  if (!isObject(v) || !isObject(v.differs)) return invalid("delta");
  for (const key of ["area_um2_median", "deformability_median", "emodulus_median_kpa"]) {
    const d = v[key];
    if (!isObject(d) || !numberOrNull(d.value)) return invalid(`delta.${key}`);
    if (d.withheld !== null && !WITHHELD.includes(d.withheld as RunDeltaWithheld)) return invalid(`delta.${key}.withheld`);
    if ((d.withheld === null) === (d.value === null)) return invalid(`delta.${key}: a value or a reason`);
  }
  for (const key of ["profile", "method_revision", "core", "contract"])
    if (!COMPARISONS.includes(v.differs[key] as RunIdentityComparison)) return invalid(`delta.differs.${key}`);
  return v as unknown as RunDelta;
}

function checkStatistics(v: unknown): RunStatistics {
  if (!isObject(v) || !isObject(v.identity) || !isObject(v.counts)) return invalid("statistics");
  const c = v.counts;
  if (!isDecimal(c.set_rows) || !isDecimal(c.valid) || !isDecimal(c.finite) || !isDecimal(c.excluded)) return invalid("counts");
  const area = v.area_um2, deform = v.deformability, e = v.emodulus, tg = v.target_group, rate = v.event_rate;
  if (!isObject(area) || !decimalOrNull(area.n) || !numberOrNull(area.median) || !numberOrNull(area.pixel_to_micron))
    return invalid("area_um2");
  if (area.pixel_to_micron === null && (area.n !== null || area.median !== null))
    return invalid("area_um2: no recorded factor, no area");
  if (!isObject(deform) || !isDecimal(deform.n) || !numberOrNull(deform.median)) return invalid("deformability");
  if (!isObject(e) || !E_STATUSES.includes(e.status as RunEmodulusStatus) || !decimalOrNull(e.n) ||
      !numberOrNull(e.median_kpa) || !decimalOrNull(e.out_of_domain)) return invalid("emodulus");
  if (e.status !== "computed" && (e.n !== null || e.median_kpa !== null || e.out_of_domain !== null))
    return invalid("emodulus: numbers only when computed");
  if (e.lut !== null && (!isObject(e.lut) ||
      !(REVIEW_EMODULUS_LUT_PROVENANCES as readonly unknown[]).includes(e.lut.provenance)))
    return invalid("emodulus.lut");
  if (e.status !== "not_recorded" && e.lut === null) return invalid("emodulus: a status beyond not_recorded needs a LUT record");
  if (!isObject(tg) || !(tg.declared === null || typeof tg.declared === "boolean") || !decimalOrNull(tg.n) ||
      !decimalOrNull(tg.of) || !numberOrNull(tg.percent)) return invalid("target_group");
  if (!isObject(rate) || !RATE_STATUSES.includes(rate.status as RunEventRateStatus) || !numberOrNull(rate.per_s))
    return invalid("event_rate");
  if (!Array.isArray(v.flags) || !v.flags.every((f) => FLAGS.includes(f as RunQuickLookFlag))) return invalid("flags");
  const o = v.overlay;
  if (!isObject(o) || !Array.isArray(o.area_px) || !Array.isArray(o.deformability) || !Array.isArray(o.target_group) ||
      o.deformability.length > RUN_QUICK_LOOK.overlay_max_points || o.area_px.length !== o.deformability.length ||
      o.target_group.length !== o.deformability.length ||
      (o.area_um2 !== null && (!Array.isArray(o.area_um2) || o.area_um2.length !== o.deformability.length)))
    return invalid("overlay");
  if (v.accounting !== null && !isObject(v.accounting)) return invalid("accounting");
  return v as unknown as RunStatistics;
}

function checkRun(v: unknown, where: string): RunQuickLookRun {
  if (!isObject(v) || typeof v.path !== "string" || typeof v.message !== "string" || typeof v.from_cache !== "boolean")
    return invalid(where);
  if (!STATUSES.includes(v.status as RunQuickLookStatus)) return invalid(`${where}.status ${String(v.status)}`);
  if (v.computed_at !== null && v.computed_at !== "close" && v.computed_at !== "on_demand") return invalid(`${where}.computed_at`);
  const computed = v.status === "computed";
  if (computed !== (v.statistics !== null)) return invalid(`${where}: statistics exactly when computed`);
  if (computed) checkStatistics(v.statistics);
  if (!computed && v.delta !== null) return invalid(`${where}: a delta only for a computed run`);
  checkDelta(v.delta);
  if (v.file !== null && (!isObject(v.file) || !isDecimal(v.file.size_bytes))) return invalid(`${where}.file`);
  return v as unknown as RunQuickLookRun;
}

/** The run_quick_look / review_run_statistics reply; anything off contract is rejected. */
export function decodeRunQuickLookReply(v: unknown): RunQuickLookReply {
  if (!isObject(v) || typeof v.ok !== "boolean" || typeof v.message !== "string" || !Array.isArray(v.runs))
    return invalid("reply");
  if (v.runs.length > RUN_QUICK_LOOK.max_paths) return invalid("runs");
  v.runs.forEach((r, i) => checkRun(r, `runs[${i}]`));
  if (!v.ok && (v.runs.length > 0 || v.reference !== null)) return invalid("a refused request carries nothing");
  if (v.reference !== null) {
    const r = v.reference;
    if (!isObject(r) || !isObject(r.requested) || typeof r.confirmed !== "boolean" || typeof r.message !== "string")
      return invalid("reference");
    if (r.run !== null) checkRun(r.run, "reference.run");
    if (r.confirmed && (!isObject(r.run) || r.run.status !== "computed")) return invalid("a confirmed reference is computed");
    if (!r.confirmed && v.runs.some((run) => isObject(run) && run.delta !== null))
      return invalid("no deltas without a confirmed reference");
  } else if (v.runs.some((run) => isObject(run) && run.delta !== null)) {
    return invalid("no deltas without a reference");
  }
  return v as unknown as RunQuickLookReply;
}

/** The arguments of run_quick_look / review_run_statistics, checked against the contract's bounds
 *  before anything is sent. */
export function runQuickLookArgs(paths: string[], reference?: RunQuickLookReference | null) {
  const pathOk = (p: unknown) => typeof p === "string" && utf8Bytes(p) > 0 && utf8Bytes(p) <= RUN_QUICK_LOOK.path_max_bytes;
  if (!Array.isArray(paths) || paths.length === 0 || paths.length > RUN_QUICK_LOOK.max_paths || !paths.every(pathOk))
    throw new Error("INVALID_PATHS");
  if (reference == null) return { paths };
  if (!pathOk(reference.path)) throw new Error("INVALID_REFERENCE_PATH");
  if (reference.run_id !== null &&
      (typeof reference.run_id !== "string" || utf8Bytes(reference.run_id) === 0 ||
       utf8Bytes(reference.run_id) > RUN_QUICK_LOOK.run_id_max_bytes)) throw new Error("INVALID_REFERENCE_RUN_ID");
  if (reference.start_generation != null && !isDecimal(reference.start_generation))
    throw new Error("INVALID_REFERENCE_START_GENERATION");
  const ref: RunQuickLookReference = { path: reference.path, run_id: reference.run_id };
  if (reference.start_generation != null) ref.start_generation = reference.start_generation;
  return { paths, reference: ref };
}
