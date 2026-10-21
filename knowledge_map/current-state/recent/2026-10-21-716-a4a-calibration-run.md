## 2026-10-21 — The calibration run: backend and bridge (#716 A4a, bridge ABI 42)

Additive under ADR 0004. A method may now declare `purpose: "calibration"` with its reference
(`calibration_reference {nominal_diameter_um, diameter_tolerance_um, nominal_e_kpa?,
max_cv_percent?}`) or, as a measurement method, a `calibration_policy` (`none` | `warn` |
`required`, `calibration_max_age_hours`, default 24); absent members keep legacy methods as they
were, and a malformed declaration refuses the config document. When a run of a calibration method
closes, [[architecture/ExperimentCoordinator]] derives a record from that run's own file (the
normal core's valid objects): median equivalent diameter px with MAD and CV, the proposed
pixel-to-micron (nominal / median), the run factor against nominal ± tolerance, the deformation
offset (report only), event-rate stability over the file's declared clock, focus only when the
file records a focus metric ("not recorded" otherwise) and E against its nominal only where both
exist. Fewer than 200 valid objects or a diameter CV above the method's limit (default 10 %)
makes the record `insufficient`, which can never be applied. Records live append-only in
`<dataDir>/instrument_calibration.json` (statuses `proposed | applied | superseded | invalidated
| insufficient`, each change a new version). `calibration_apply(record_id,
expected_current_factor)` is the one explicit compare-and-set: inside the coordinator's idle
transaction, refused during a run, for a stale factor, another setup or an insufficient record;
it supersedes the previous applied record, sets the factor and replies previous vs new; it never
touches the method. The readiness gate `calibration.record` follows the method's policy (hard
invalidation on a camera / ROI identity change, soft age warning, required blocks Start, Fix
names the real action). Start records the applied record in effect as the snapshot v3's
`calibration_record_id` and refuses a declared id that differs (`InvalidRequest`, mismatch).
New commands: `calibration_records_list`, `calibration_record_get` (reads) and
`calibration_apply` (a control command on the WebSocket server). See
[[services/CalibrationRecords]], [[architecture/Rust-Bridge]] (ABI 42),
[[data-model/HDF5-Storage]], [[architecture/Desktop-Shell]]. The workspace Prepare card is A4b.
Guards: `backend.calibration_record`, `e2e.calibration_run`, `backend.experiment_readiness`
(the mismatch), `contract.rs`, `dispatch.rs`, `ws.rs`, `calibrationRecords.test.ts`.
