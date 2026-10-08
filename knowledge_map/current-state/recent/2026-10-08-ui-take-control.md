## 2026-10-08 — A viewer-only browser says so and can take control

Found in the first Studio slot on the PZ7035: after a restart an older tab reconnected first and held
the controller role, so every other browser's mode switches and starts failed with `VIEWER_ONLY` and
only a log line said so. The transport now tracks the server's session messages and `ControlBanner`
shows "Another client controls this instrument" with a Take control button. Verified with two
Chromium pages against a host server. See [[architecture/Desktop-Shell]].
