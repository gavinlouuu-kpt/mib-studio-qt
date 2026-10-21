// Calibration records (bridge ABI 42, #716 A4a; bridge-contract.json `calibration_records`), and
// the applied method's calibration declarations in the readiness report (ABI 43, #776).
//
// The backend derives a record when a run of a calibration method closes, keeps every version in
// the instrument settings store and applies pixel-to-micron only on an explicit
// `calibration_apply` (a compare-and-set against the factor the client saw). Listing and reading
// never apply anything. This module checks the replies' shape; it computes nothing: every number
// is the backend's, and an unknown value stays null (never 0).
import { CALIBRATION_APPLY_OUTCOMES, CALIBRATION_RECORDS } from "./bridgeContract";

export type CalibrationRecordStatus = "proposed" | "applied" | "superseded" | "invalidated" | "insufficient";
const STATUSES: readonly CalibrationRecordStatus[] = ["proposed", "applied", "superseded", "invalidated", "insufficient"];

export interface CalibrationIdentity {
  camera: string | null;
  roi: string | null;
  chip: string;
  objective: string;
}

/** One version of a record, as the backend serialises it (contract `calibration_records.record`). */
export interface CalibrationRecord {
  schema_version: number;
  record_id: string;
  version: number;
  sequence: number;
  status: CalibrationRecordStatus;
  status_reason: string;
  status_at_utc: string;
  created_at_utc: string;
  applicable: boolean;
  source: { run_id: string | null; file: string; file_sha256: string | null; start_generation: number };
  method: Record<string, unknown>;
  inputs: Record<string, number | null>;
  identity: CalibrationIdentity;
  results: Record<string, unknown> & {
    valid_objects: number;
    proposed_pixel_to_micron: number | null;
    focus: { status: string; median: number | null };
  };
  gates: { id: string; pass: boolean; value: number | null; limit: number; reason: string | null }[];
  apply: { previous_factor: number | null; new_factor: number | null } | null;
  superseded_by: string | null;
}

export interface CalibrationRecordsList {
  valid: boolean;
  store?: { revision: number; max_versions: number; error: string | null };
  applied_record_id?: string | null;
  in_effect_record_id?: string | null;
  current_factor?: number | null;
  current_identity?: CalibrationIdentity & { known: boolean };
  records?: CalibrationRecord[];
}

export interface CalibrationRecordGet {
  found: boolean;
  message: string;
  record: CalibrationRecord | null;
  history: CalibrationRecord[];
}

export type CalibrationApplyOutcome = (typeof CALIBRATION_APPLY_OUTCOMES)[keyof typeof CALIBRATION_APPLY_OUTCOMES];

export interface CalibrationApplyReply {
  ok: boolean;
  outcome: CalibrationApplyOutcome;
  outcome_name: string;
  message: string;
  record_id: string;
  previous_factor: number | null;
  new_factor: number | null;
  superseded_record_id: string | null;
  record: CalibrationRecord | null;
}

export type MethodPurpose = "measurement" | "calibration";
export type CalibrationPolicy = "none" | "warn" | "required";
const PURPOSES: readonly MethodPurpose[] = ["measurement", "calibration"];
const POLICIES: readonly CalibrationPolicy[] = ["none", "warn", "required"];

/** A calibration method's reference sample (the method document's `calibration_reference`). */
export interface MethodCalibrationReference {
  nominal_diameter_um: number;
  diameter_tolerance_um: number;
  /** Check-only; null when the method declares none. */
  nominal_e_kpa: number | null;
  /** The diameter CV limit, the default (10) filled in. */
  max_cv_percent: number;
}

/**
 * The applied method's calibration declarations, in the readiness report as `method_calibration`
 * (ABI 43, #776; contract `calibration_records.method_calibration`), exactly as that evaluation's
 * `calibration.record` gate read them. With no method applied, or one that declares nothing:
 * `declared` false, measurement / none / the default age, no reference. `calibration_reference` is
 * set exactly for a calibration method. `error` is non-null only when the applied declarations
 * could not be read (the gate then fails closed; purpose and policy are the defaults).
 */
export interface MethodCalibration {
  declared: boolean;
  purpose: MethodPurpose;
  calibration_policy: CalibrationPolicy;
  calibration_max_age_hours: number;
  calibration_reference: MethodCalibrationReference | null;
  error: string | null;
}

const invalid = (why: string): never => {
  throw new Error(`CALIBRATION_REPLY_INVALID: ${why}`);
};
const isObject = (v: unknown): v is Record<string, unknown> => typeof v === "object" && v !== null && !Array.isArray(v);

function checkRecord(v: unknown): CalibrationRecord {
  if (!isObject(v)) return invalid("a record is not an object");
  if (typeof v.record_id !== "string" || v.record_id.length === 0) return invalid("record_id");
  if (!STATUSES.includes(v.status as CalibrationRecordStatus)) return invalid(`status ${String(v.status)}`);
  if (typeof v.applicable !== "boolean" || !isObject(v.results) || !isObject(v.identity)) return invalid("record shape");
  return v as unknown as CalibrationRecord;
}

/** calibration_records_list: `{valid: false}` before init. */
export function decodeCalibrationRecordsList(v: unknown): CalibrationRecordsList {
  if (!isObject(v) || typeof v.valid !== "boolean") return invalid("list");
  if (!v.valid) return { valid: false };
  if (!Array.isArray(v.records) || v.records.length > CALIBRATION_RECORDS.store_max_versions) return invalid("records");
  v.records.forEach(checkRecord);
  return v as unknown as CalibrationRecordsList;
}

export function decodeCalibrationRecordGet(v: unknown): CalibrationRecordGet {
  if (!isObject(v) || typeof v.found !== "boolean" || !Array.isArray(v.history)) return invalid("get");
  if (v.found) checkRecord(v.record);
  v.history.forEach(checkRecord);
  return v as unknown as CalibrationRecordGet;
}

/** calibration_apply's reply; an unknown outcome is rejected (contract: reject and reconcile). */
export function decodeCalibrationApplyReply(v: unknown): CalibrationApplyReply {
  if (!isObject(v) || typeof v.ok !== "boolean") return invalid("apply");
  const outcomes: readonly number[] = Object.values(CALIBRATION_APPLY_OUTCOMES);
  if (typeof v.outcome !== "number" || !outcomes.includes(v.outcome)) return invalid(`outcome ${String(v.outcome)}`);
  if (v.ok !== (v.outcome === CALIBRATION_APPLY_OUTCOMES.Applied)) return invalid("ok does not match the outcome");
  if (v.record !== null && v.record !== undefined) checkRecord(v.record);
  return v as unknown as CalibrationApplyReply;
}

/** The arguments of calibration_apply, checked before anything is sent. */
export function calibrationApplyArgs(recordId: string, expectedCurrentFactor: number) {
  const bytes = new TextEncoder().encode(recordId).length;
  if (bytes === 0 || bytes > CALIBRATION_RECORDS.record_id_max_bytes) throw new Error("INVALID_RECORD_ID");
  if (!Number.isFinite(expectedCurrentFactor) || expectedCurrentFactor <= 0) throw new Error("INVALID_EXPECTED_FACTOR");
  return { recordId, expectedCurrentFactor };
}

const finite = (v: unknown): v is number => typeof v === "number" && Number.isFinite(v);

/** The readiness report's `method_calibration` (ABI 43): every member is required; an unknown
 *  purpose or policy, a reference on a measurement method or a calibration method without one is
 *  rejected (contract: reject and reconcile). Nothing is defaulted on the client. */
export function decodeMethodCalibration(v: unknown): MethodCalibration {
  if (!isObject(v) || typeof v.declared !== "boolean") return invalid("method_calibration");
  if (!PURPOSES.includes(v.purpose as MethodPurpose)) return invalid(`purpose ${String(v.purpose)}`);
  if (!POLICIES.includes(v.calibration_policy as CalibrationPolicy)) return invalid(`calibration_policy ${String(v.calibration_policy)}`);
  if (!finite(v.calibration_max_age_hours) || v.calibration_max_age_hours <= 0) return invalid("calibration_max_age_hours");
  if (v.error !== null && typeof v.error !== "string") return invalid("error");
  const ref = v.calibration_reference;
  let reference: MethodCalibrationReference | null = null;
  if (ref !== null) {
    if (!isObject(ref) || !finite(ref.nominal_diameter_um) || !finite(ref.diameter_tolerance_um) || !finite(ref.max_cv_percent) ||
        (ref.nominal_e_kpa !== null && !finite(ref.nominal_e_kpa))) return invalid("calibration_reference");
    reference = { nominal_diameter_um: ref.nominal_diameter_um, diameter_tolerance_um: ref.diameter_tolerance_um,
      nominal_e_kpa: ref.nominal_e_kpa as number | null, max_cv_percent: ref.max_cv_percent };
  }
  if ((v.purpose === "calibration") !== (reference !== null)) return invalid("a reference is declared exactly by a calibration method");
  return { declared: v.declared, purpose: v.purpose as MethodPurpose, calibration_policy: v.calibration_policy as CalibrationPolicy,
    calibration_max_age_hours: v.calibration_max_age_hours, calibration_reference: reference, error: v.error as string | null };
}
