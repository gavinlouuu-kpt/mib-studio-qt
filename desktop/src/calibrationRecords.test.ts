// Bridge ABI 42 (#716 A4a): the calibration records client against a mocked transport. The list,
// get and apply replies are checked against the contract, an unknown status or outcome is
// rejected, and a malformed apply never reaches the transport. ABI 43 (#776): the readiness
// report's method_calibration is decoded strictly.
import { beforeEach, expect, it, vi } from "vitest";
import { bridge } from "./bridge";
import { CALIBRATION_APPLY_OUTCOMES, CALIBRATION_RECORDS, CALIBRATION_RECORD_STATUSES } from "./bridgeContract";
import { decodeMethodCalibration } from "./calibrationRecords";

const { invoke } = vi.hoisted(() => ({ invoke: vi.fn() }));
vi.mock("@tauri-apps/api/core", () => ({ invoke }));
beforeEach(() => { invoke.mockReset(); });

const record = (extra: Record<string, unknown> = {}) => ({
  schema_version: 1, record_id: "cal-20261021T093000Z-1a2b3c4d", version: 1, sequence: 3, status: "proposed",
  status_reason: "derived from run 4; a proposal until applied", status_at_utc: "2026-10-21T09:30:00Z",
  created_at_utc: "2026-10-21T09:30:00Z", applicable: true,
  source: { run_id: "3f2a9c1e-7b4d-4e8a-9c2f-1a2b3c4d5e6f", file: "/data/beads.h5", file_sha256: "ab", start_generation: 4 },
  method: { source: "local" }, inputs: { nominal_diameter_um: 7.32, diameter_tolerance_um: 0.1 },
  identity: { camera: "mock:frames", roi: "full", chip: "not tracked", objective: "not tracked" },
  results: { valid_objects: 400, proposed_pixel_to_micron: 0.488, focus: { status: "not recorded", median: null } },
  gates: [{ id: "min_valid_objects", pass: true, value: 400, limit: 200, reason: null }],
  apply: null, superseded_by: null, ...extra,
});

it("pins the contract's statuses, outcomes and bounds", () => {
  expect(CALIBRATION_RECORD_STATUSES).toEqual({ Proposed: 0, Applied: 1, Superseded: 2, Invalidated: 3, Insufficient: 4 });
  expect(CALIBRATION_APPLY_OUTCOMES).toEqual({
    Applied: 0, NotFound: 1, Insufficient: 2, NotApplicable: 3, StaleFactor: 4, RunActive: 5, IdentityMismatch: 6,
    StoreFailed: 7, InvalidRequest: 8,
  });
  expect(CALIBRATION_RECORDS).toMatchObject({ record_id_max_bytes: 128, min_valid_objects: 200, store_max_versions: 2048 });
});

it("lists and reads records; listing sends no arguments", async () => {
  invoke.mockResolvedValueOnce({
    valid: true, store: { revision: 3, max_versions: 2048, error: null }, applied_record_id: null, in_effect_record_id: null,
    current_factor: 0.4886, current_identity: { camera: "mock:frames", roi: "full", chip: "not tracked", objective: "not tracked", known: true },
    records: [record(), record({ record_id: "cal-2", status: "insufficient", applicable: false })],
  });
  const list = await bridge.calibrationRecordsList();
  expect(invoke).toHaveBeenLastCalledWith("calibration_records_list");
  expect(list.records?.map(r => r.status)).toEqual(["proposed", "insufficient"]);
  invoke.mockResolvedValueOnce({ valid: false });
  expect(await bridge.calibrationRecordsList()).toEqual({ valid: false });
  invoke.mockResolvedValueOnce({ valid: true, records: [record({ status: "approved" })] });
  await expect(bridge.calibrationRecordsList()).rejects.toThrow(/CALIBRATION_REPLY_INVALID: status approved/);

  invoke.mockResolvedValueOnce({ found: true, message: "", record: record(), history: [record()] });
  const got = await bridge.calibrationRecordGet("cal-1");
  expect(invoke).toHaveBeenLastCalledWith("calibration_record_get", { recordId: "cal-1" });
  expect(got.found && got.history.length).toBe(1);
});

it("applies with previous vs new; refusals and unknown outcomes", async () => {
  invoke.mockResolvedValueOnce({
    ok: true, outcome: 0, outcome_name: "applied", message: "record cal-1 applied", record_id: "cal-1",
    previous_factor: 0.4886, new_factor: 0.488, superseded_record_id: null,
    record: record({ status: "applied", version: 2, applicable: false, apply: { previous_factor: 0.4886, new_factor: 0.488 } }),
  });
  const r = await bridge.calibrationApply("cal-1", 0.4886);
  expect(invoke).toHaveBeenLastCalledWith("calibration_apply", { recordId: "cal-1", expectedCurrentFactor: 0.4886 });
  expect([r.previous_factor, r.new_factor]).toEqual([0.4886, 0.488]);

  invoke.mockResolvedValueOnce({ ok: false, outcome: 4, outcome_name: "staleFactor", message: "stale", record_id: "cal-1",
    previous_factor: 0.5, new_factor: null, superseded_record_id: null, record: null });
  expect((await bridge.calibrationApply("cal-1", 0.4886)).outcome).toBe(CALIBRATION_APPLY_OUTCOMES.StaleFactor);
  invoke.mockResolvedValueOnce({ ok: false, outcome: 42, message: "?", record: null });
  await expect(bridge.calibrationApply("cal-1", 0.4886)).rejects.toThrow(/outcome 42/);
  invoke.mockResolvedValueOnce({ ok: true, outcome: 2, message: "?", record: null });
  await expect(bridge.calibrationApply("cal-1", 0.4886)).rejects.toThrow(/ok does not match/);

  invoke.mockClear();
  await expect(bridge.calibrationApply("", 0.4886)).rejects.toThrow("INVALID_RECORD_ID");
  await expect(bridge.calibrationApply("c".repeat(129), 0.4886)).rejects.toThrow("INVALID_RECORD_ID");
  await expect(bridge.calibrationApply("cal-1", Number.NaN)).rejects.toThrow("INVALID_EXPECTED_FACTOR");
  await expect(bridge.calibrationApply("cal-1", 0)).rejects.toThrow("INVALID_EXPECTED_FACTOR");
  expect(invoke).not.toHaveBeenCalled();
});

// ABI 43 (#776): the applied method's calibration declarations in the readiness report.
const readinessReply = (method_calibration: unknown) => ({
  transport_version: 1, valid: true, ready: true, generation: "7",
  gates: [{ id: "calibration.record", status: 4, reason: "a calibration method derives its own record", remediation: "" }],
  method_calibration,
});
const calibrationMethod = {
  declared: true, purpose: "calibration", calibration_policy: "none", calibration_max_age_hours: 24, error: null,
  calibration_reference: { nominal_diameter_um: 7.32, diameter_tolerance_um: 0.1, nominal_e_kpa: null, max_cv_percent: 4 },
};
const undeclared = {
  declared: false, purpose: "measurement", calibration_policy: "none", calibration_max_age_hours: 24,
  calibration_reference: null, error: null,
};

it("decodes a calibration method's purpose and reference from the readiness report", async () => {
  invoke.mockResolvedValueOnce(readinessReply(calibrationMethod));
  const r = await bridge.fetchExperimentReadiness("/data/beads.h5");
  expect(invoke).toHaveBeenLastCalledWith("fetch_experiment_readiness", { outputPath: "/data/beads.h5" });
  expect(r.method_calibration).toEqual(calibrationMethod);
  expect(r.method_calibration?.calibration_reference?.nominal_e_kpa).toBeNull();
});

it("reports measurement / none with declared false, never missing members", async () => {
  invoke.mockResolvedValueOnce(readinessReply(undeclared));
  const r = await bridge.fetchExperimentReadiness("/data/run.h5");
  expect(r.method_calibration).toEqual(undeclared);
  const gated = { ...undeclared, declared: true, calibration_policy: "required", calibration_max_age_hours: 12 };
  expect(decodeMethodCalibration(gated)).toEqual(gated);
  expect(decodeMethodCalibration({ ...undeclared, error: "calibration_policy: \"always\" is not ..." }).error).toContain("always");
});

it("rejects a malformed method_calibration", async () => {
  const bad: unknown[] = [
    null, {}, { ...undeclared, declared: "no" }, { ...undeclared, purpose: "qc" }, { ...undeclared, calibration_policy: "always" },
    { ...undeclared, calibration_max_age_hours: 0 }, { ...undeclared, calibration_max_age_hours: null },
    { ...undeclared, error: 3 }, { ...undeclared, calibration_reference: calibrationMethod.calibration_reference },
    { ...calibrationMethod, calibration_reference: null },
    { ...calibrationMethod, calibration_reference: { ...calibrationMethod.calibration_reference, nominal_diameter_um: "7" } },
    { ...calibrationMethod, calibration_reference: { ...calibrationMethod.calibration_reference, nominal_e_kpa: Number.NaN } },
    // The method document's bounds (Codex round 1): the backend never sends these.
    { ...undeclared, calibration_max_age_hours: 87601 }, { ...undeclared, calibration_max_age_hours: -1 },
    ...[{ nominal_diameter_um: 0 }, { nominal_diameter_um: -7 }, { nominal_diameter_um: 1001 }, { diameter_tolerance_um: -0.1 },
      { diameter_tolerance_um: 8 }, { nominal_e_kpa: 0 }, { nominal_e_kpa: -3 }, { nominal_e_kpa: 1e6 + 1 },
      { max_cv_percent: 0 }, { max_cv_percent: 101 }]
      .map(r => ({ ...calibrationMethod, calibration_reference: { ...calibrationMethod.calibration_reference, ...r } })),
  ];
  for (const v of bad) expect(() => decodeMethodCalibration(v), JSON.stringify(v)).toThrow("CALIBRATION_REPLY_INVALID");
  // The bounds themselves are accepted.
  const edge = { ...calibrationMethod, calibration_max_age_hours: 87600,
    calibration_reference: { nominal_diameter_um: 1000, diameter_tolerance_um: 1000, nominal_e_kpa: 1e6, max_cv_percent: 100 } };
  expect(decodeMethodCalibration(edge)).toEqual(edge);
  expect(decodeMethodCalibration({ ...edge, calibration_reference: { ...edge.calibration_reference, diameter_tolerance_um: 0 } })
    .calibration_reference?.diameter_tolerance_um).toBe(0);
  // Members missing entirely are an error too: the backend always sends every one.
  const { error: _omitted, ...missing } = undeclared;
  expect(() => decodeMethodCalibration(missing)).toThrow("CALIBRATION_REPLY_INVALID");
  invoke.mockResolvedValueOnce(readinessReply({ ...undeclared, purpose: "qc" }));
  await expect(bridge.fetchExperimentReadiness("/data/run.h5")).rejects.toThrow("CALIBRATION_REPLY_INVALID");
});
