## 2026-10-07 — Freeze background and ROI setup during experiments (#542)

Background set/clear and asynchronous calibration publication use the coordinator
idle configuration transaction. React disables setup during Starting/Active/Stopping
and refreshes authoritative ROI/background values after accepted or refused commands.
Facade, calibration and React regressions cover refusal and idle operation.
See [[architecture/ExperimentCoordinator]], [[architecture/Desktop-Shell]]
and [[services/ProcessingService]].
