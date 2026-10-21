## 2026-10-22 — Readiness reports the applied method's calibration declarations (#776, bridge ABI 43)

Follow-up from the A4b Prepare card (#775): the workspace could not tell from the bridge that the
applied method is a calibration method, so the card said so in general terms and showed the
`calibration.record` gate in the backend's own words.

The readiness report now carries `method_calibration`: the applied method's `purpose`,
`calibration_policy`, `calibration_max_age_hours` and, for a calibration method, its
`calibration_reference` (nominal diameter and tolerance in µm, optional nominal E, CV limit),
exactly as that evaluation's gate read them. With no method applied, or a method that declares
none of them, it reports `declared: false`, measurement, none, 24 h and no reference, never missing
members. It is a separate member of the report, not the gate's detail, because a readiness gate on
the bridge has no structured details (`{id, status, reason, remediation}` for every gate).

C++: `CalibrationMethodSpec.h` (split from `CalibrationRecord.h`), `declared` set by
`parseCalibrationMethodSpec`, `ExperimentReadinessSnapshot::methodCalibration`. Bridge: cxx
`BridgeMethodCalibration`, `mib-app-commands` `MethodCalibration` (Tauri and the WebSocket server),
`eventAdapter` / `calibrationRecords.ts` `decodeMethodCalibration`. Additive under ADR 0004; ABI 43.
No UI on develop: the workspace card switches to it after the next sync. See
[[services/CalibrationRecords]] and [[architecture/Rust-Bridge]].
