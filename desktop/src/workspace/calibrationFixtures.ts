// Test fixtures for the calibration card (#716 A4b): records and list replies shaped as the backend
// serialises them (CalibrationRecord.cpp calibrationRecordToJson, BackendFacade
// calibrationRecordsListJson). Used by the tests only.
import type { CalibrationIdentity, CalibrationRecord, CalibrationRecordStatus, CalibrationRecordsList } from "../calibrationRecords";

export const SETUP: CalibrationIdentity = { camera: "mock:Camera 01", roi: "full", chip: "not tracked", objective: "not tracked" };

/** One record version; `over` replaces members (results and gates merge shallowly). */
export function calRecord(id: string, status: CalibrationRecordStatus, over: Partial<CalibrationRecord> & { factor?: number } = {}): CalibrationRecord {
  const { factor = 0.52, ...rest } = over;
  const insufficient = status === "insufficient";
  return {
    schema_version: 1, record_id: id, version: 1, sequence: 1, status, status_reason: "", status_at_utc: "2026-10-21T08:00:00Z",
    created_at_utc: "2026-10-21T07:00:00Z", applicable: status === "proposed",
    source: { run_id: "run-1", file: `/data/${id}.h5`, file_sha256: "ab", start_generation: 3 },
    method: { source: "local", method_id: "beads-7um", revision_id: "r3", revision_number: 3, display_name: "Beads 7 µm", config_sha256: "cd" },
    inputs: { nominal_diameter_um: 7, diameter_tolerance_um: 0.5, nominal_e_kpa: null, max_cv_percent: 10, min_valid_objects: 200, rate_window_s: 1, run_pixel_to_micron: 0.5 },
    identity: { ...SETUP },
    results: {
      valid_objects: insufficient ? 120 : 812, proposed_pixel_to_micron: insufficient ? null : factor, cv_diameter_percent: insufficient ? 14.2 : 3.1,
      median_diameter_px: 13.4, focus: { status: "not recorded", median: null },
    },
    gates: insufficient
      ? [{ id: "min_valid_objects", pass: false, value: 120, limit: 200, reason: null }, { id: "max_cv_percent", pass: false, value: 14.2, limit: 10, reason: null }]
      : [{ id: "min_valid_objects", pass: true, value: 812, limit: 200, reason: null }, { id: "max_cv_percent", pass: true, value: 3.1, limit: 10, reason: null }],
    apply: status === "applied" ? { previous_factor: 0.5, new_factor: factor } : null,
    superseded_by: null,
    ...rest,
  };
}

/** A list reply: `records` newest first. */
export function calList(records: CalibrationRecord[], over: Partial<CalibrationRecordsList> = {}): CalibrationRecordsList {
  return {
    valid: true, store: { revision: records.length, max_versions: 2048, error: null },
    applied_record_id: records.find((r) => r.status === "applied")?.record_id ?? null, in_effect_record_id: null, current_factor: 0.5,
    current_identity: { ...SETUP, known: true }, records, ...over,
  };
}

/** An apply reply. */
export function applyReply(recordId: string, outcome: number, over: Record<string, unknown> = {}) {
  return {
    ok: outcome === 0, outcome, outcome_name: "", message: "", record_id: recordId, previous_factor: null, new_factor: null,
    superseded_record_id: null, record: null, ...over,
  };
}
