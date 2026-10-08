## 2026-10-08 — Dropped frames warn only while the results bridge is armed

On the board, P[6] (dropped) rose by about 400/s at the Align timing with nothing consuming the
bridge, so `dropped_warn` (sustained above 0.1% of the frame rate) would have warned in every Run
without an experiment. `PzPlatformMonitor` now reads the bridge state and reports `link.bridge_active`
(ARMED or RUNNING); the dropped-frame warning needs it, and its clock restarts when the bridge stops.
Bad frames are ingress-level and unaffected. See [[architecture/Desktop-Shell]].
