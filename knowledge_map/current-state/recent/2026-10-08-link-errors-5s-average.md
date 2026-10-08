## 2026-10-08 — Ingress error warning is a 5 s average

The Run 5 kHz baseline measured on the board (2026-10-08) is 1.12 errors/s in bursts of 8–13, 8.9×
under the 10/s threshold, so a single-second rate could warn on a healthy link. `PzPlatformMonitor`
now averages P[12] over the last 5 s (`ingress_errors_avg_per_s`) and warns above 10/s
(`ingress_errors_warn`) only after a full window since the last mode-switch settle; the UI Link row
and the preflight Sensor link check use the flag. A burst of 13 averages to about 2.4/s and does not
warn; 12/s sustained does. Resyncs, bad and dropped frames are unchanged. See [[Desktop-Shell]].
