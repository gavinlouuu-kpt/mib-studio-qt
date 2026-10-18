## 2026-10-19 — Start request declares experiment, run, sample, operator, calibration and annotation (#712, ABI 39)

`experiment_start` takes an optional `startRequest` (bridge ABI 39): the experiment id (UUID), the run's index in
it, the sample-sheet entry, the operator the client selected (#393 phase 1, recorded with `assurance: "selected"`,
not authentication), the applied calibration record id (ADR 0015 item 7) and the planned annotation (condition,
experimental unit, concentration with unit, sample, notes, the declared channel / medium / temperature, and free
string key/values). The backend checks it strictly (`recording::parseStartRequestJson` in the facade,
`validateStartRequest` in the coordinator): unknown or duplicate keys, wrong types, `""`, control characters,
invalid UTF-8 and anything over a contract bound (`experiment_start_request`) are refused with the key named and
start outcome `InvalidRequest` (7), never truncated, and nothing is opened. The identity goes into the run snapshot
v3, the annotation once into `/experiment_annotation` (a failed write is `ProvenanceFailed`, the file removed).
Not declared is `null` and reads back as unknown; older files and runs started without an annotation have no
block. No stop-condition change (that is a later ABI). See [[architecture/Rust-Bridge]],
[[data-model/HDF5-Storage]], [[services/Hdf5Service]], [[architecture/ExperimentCoordinator]]. Guards:
`recording.run_start_identity`, `backend.experiment_readiness`, `contract.rs`
`experiment_lifecycle_end_to_end` and `experiment_start_request_bounds_are_pinned`.
