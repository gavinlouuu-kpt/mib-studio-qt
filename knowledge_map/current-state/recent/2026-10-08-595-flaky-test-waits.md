## 2026-10-08 — Stabilize five CI test harnesses (#595)

Test-only fixes: drain inline cursor replay and wait for a buffered failure batch; run KDE phases by completed frame/estimate work with bounded progress waits; tolerate two isolated trigger scheduler outliers while retaining zero missed pulses; hold a fake stage transaction until teardown contenders queue; allow 100 ms of export-soak filesystem noise while retaining object and integrity checks.

See [[services/ProcessingService]], [[services/MonitoringDensityService]], [[services/TriggerService]], [[services/ZC300Stage]] and [[services/HdfExportService]]. No runtime behavior or bridge ABI changes.
