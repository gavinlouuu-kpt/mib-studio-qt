## 2026-10-22 — #807 recorded calibration history and native qualification

Prepare's existing CalibrationCard now mounts `CalibrationTrends.tsx`, a
read-only view of the real calibration-list adapter. `calibrationTrendModel.ts`
keeps latest record versions, derivation-time ordering, recorded limits and
verdicts, exact source refs and missing focus/flow reasons. It does not
recompute statistics, apply calibration, persist exclusions or alter thresholds.
The all-record chart is explicitly unpooled; setup filters include method
revision and never assume incomplete identities are compatible. Selecting a
point uses the existing record-get/version panel. Failed reads clear the chart.

Nine real native backend tests passed locally (synthetic/mock HDF5/SQLite
inputs), covering start identity, expected-run stop, coordinator, Quick look,
review statistics, calibration/store faults, application and method gates.
See `docs/design/ique-workflow/NATIVE-EVIDENCE.md` and
[[architecture/Desktop-Shell]]. This is partial I11 evidence, not hardware or
complete iQue acceptance. The owner directed continued execution while useful
work remains; peer API gaps are not a reason to stop independent slices.
