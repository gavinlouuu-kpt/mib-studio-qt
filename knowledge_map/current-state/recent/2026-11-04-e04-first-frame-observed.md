## 2026-11-04 — E04 first-frame observation event

Capture lifecycle snapshots now distinguish accepted start, camera readiness,
and first valid frame observation. The generation-scoped event fires once after
preview publication commits (when a FrameStore is configured) and only while the
session is still Running, so a frame in flight during `stop()` is not reported.
Lifecycle JSON exposes retained observation metadata. After the latch the push
loop skips the observation block (no `lifecycleMutex_` or `committedCount()`
reads), but it still copies the frame callback and store under `callbackMutex_` on
every frame, which costs tens of nanoseconds per frame. The capture thread also logs
one info line per generation (`CaptureService first frame observed (generation=..,
index=.., host_us=..)`) so a bench run can observe the event without a lifecycle
reader. The guided E04 tour is still a delivery gap because it is not present in
this checkout. See [[CaptureService]].
