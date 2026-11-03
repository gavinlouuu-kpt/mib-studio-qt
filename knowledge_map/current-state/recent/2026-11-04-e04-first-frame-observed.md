## 2026-11-04 — E04 first-frame observation event

Capture lifecycle snapshots now distinguish accepted start, camera readiness,
and first valid frame observation. The generation-scoped event fires once after
preview publication commits (when a FrameStore is configured) and only while the
session is still Running, so a frame in flight during `stop()` is not reported.
Lifecycle JSON exposes retained observation metadata. The guided E04 tour is still a
delivery gap because it is not present in this checkout. See [[CaptureService]].
