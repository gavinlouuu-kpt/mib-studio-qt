## 2026-10-08 — Link health, the sensor's real fps and geometry, and latency over budget in Studio

Items 3 and 4 of the board owner's list for retiring the bench viewer. `fetch_instrument_status`
gains `sensor{xvs_period_clocks, fps, width, height}` (S[9] and S[29]); the UI's PL core section
shows Sensor, Link (errors, resyncs, bad and dropped frames per second) and Latency (max and frames
over budget), and the preflight Sensor link check lists the rates and the sensor. The rates are
reported invalid for 1.5 s after a camera mode switch so the reset's counter jump never warns
(`PzPlatformMonitor::settle`). Bridge ABI unchanged (additive JSON). See
[[architecture/Desktop-Shell]], [[data-model/PZ7035-Records]].
