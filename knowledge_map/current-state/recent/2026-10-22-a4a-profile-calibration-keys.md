## 2026-10-22 — Method apply carries the calibration declarations, never stale ones (#716 A4a)

Found by the A4b click-through on PR #775. The `calibration.record` gate (#771, bridge ABI 42)
reads the method's `purpose`, `calibration_policy`, `calibration_max_age_hours` and
`calibration_reference` from the applied config text. `ProfileStore::apply` builds that text
from the previous provenance plus a fixed list of keys copied from the profile, and the
calibration members were not on the list. A local calibration profile therefore never reached
the gate (policy none, `required` not enforced, no record derived at close), and declarations
from an earlier applied method carried over to a profile that declares none, so a measurement
method could inherit `purpose: calibration` or a policy.

`ProfileStore::apply` now replaces the calibration members (`kCalibrationDeclarationKeys` in
`CalibrationRecord.h`) with the profile's own: copied verbatim when declared (the strict parse in
`stageConfigDocument` already accepted them), removed when absent. The other apply paths already
followed that rule and are unchanged: the central method stores its full text, and processing
settings (`ProcessingSettingsCommand`, `applyProcessingConfigTransaction`) keep the current
method's declarations. No ABI change. Pinned by `e2e.calibration_profile_apply`, which goes
through the real profile directory and apply command. See [[services/CalibrationRecords]].
