## 2026-10-09 — No sensor-link drop warning in Align

On the board (pl-results12, bundle ec3bcf22) every Align session started with "Needs attention": the Preflight Sensor link check
warned "dropped frames above 0.1% of the frame rate for 5 s". P[6] is the frames-dropped counter of the streaming U-Net path, which
starts only in cell mode (Run); in Align the bridge is armed for previews but the path is off, so every frame counts as dropped
(about 400/s, the sensor rate; the board owner confirmed this reading). `PzPlatformMonitor` now raises the dropped-frames warning only
while the cell path is on (S[46] and P[8] bit 4, the levels `PzInstrumentControl::setCellPath` writes; `cellPathOn` in the status).
`pz_platform_monitor_test` covers Align with a counter at the frame rate (no warning), Run with no drops (none), Run with drops above
0.1 % for 5 s (warns) and back to Align (clears). Bad frames, ingress errors and resyncs are unchanged. See [[Desktop-Shell]].
