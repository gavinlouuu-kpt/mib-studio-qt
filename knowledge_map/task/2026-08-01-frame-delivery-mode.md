Title: Explicit Every Frame / Latest Frame camera delivery modes

Context:
- Epic #328 (issues #329–#334): users could not tell whether the pipeline
  preserves every frame or prioritizes freshness, and no layer controlled the
  camera SDK's own completed-buffer queue. EGrabber's 20 announced buffers
  acted as a hidden latency reservoir under overload; MindVision only dropped
  frames after the SDK dequeue + copy.
- New contract in `camera::common` ([[../camera/ICamera]]):
  `FrameDeliveryMode { EveryFrame, LatestFrame }`,
  `FrameDeliveryCapabilities`, `AcquisitionQueueStats` (intentional discards,
  transport loss, underruns, queue depths — never merged; `*Valid` flags mark
  unobservable fields).

Implementation Notes:
- [[../services/CaptureService]] plumbs the requested mode, rejects
  unsupported modes with an actionable error before camera start, records the
  backend-confirmed mode (`activeDeliveryMode()`), polls
  `AcquisitionQueueStats` on the 1 s stats interval, computes frame age when
  the backend flags host-comparable timestamps, and warns on EveryFrame
  backlog growth.
- [[../camera/EGrabberCamera]]: single `grabber->start()` (data stream before
  remote `AcquisitionStart`, no 50 ms camera-first window); LatestFrame
  drains stale completed buffers via ScopedBuffer pops (never
  `flushEvent<NewBufferData>()` alone) with `BufferPartCount` forced to 1;
  telemetry from `STREAM_INFO_NUM_AWAIT_DELIVERY/NUM_QUEUED/NUM_UNDERRUN/
  NUM_DELIVERED`.
- [[../camera/MindVisionCamera]]: newest-priority retrieval
  (`CameraGetImageBufferPriority` + `CAMERA_GET_IMAGE_PRIORITY_NEWEST`) when
  the SDK header defines it (compile-time detection), otherwise a bounded
  non-blocking drain that releases each stale buffer exactly once and counts
  it; queue depth/underrun fields explicitly marked unavailable.
- [[../camera/MockCamera]]: pull source — both modes supported, zero queue.
- Persistence: `camera.frame_delivery_mode` in profile `config.json`
  (`"everyFrame"` default; unknown values migrate deterministically to
  EveryFrame), parsed by AppConfigWatcher into the first-ever
  `CaptureService::setConfig` call site.
- UI ([[../frontend/MainWindow]], [[../frontend/ConnectTab]]): persistent
  status-bar badge (text + glyph, backend-confirmed mode), delivery-mode
  combo on ConnectTab with per-profile write-back, blocking acknowledgement
  when starting an experiment in Latest Frame (recommended action: switch to
  Every Frame).
- Tests: `tests/support/queue_camera.h` (simulated SDK FIFO with full frame
  accounting) + `tests/camera/delivery_mode_overload_test.cpp` and
  `delivery_mode_contract_test.cpp`; hardware runbook
  `docs/howto/camera-latency-mode-validation.md`.

Logging:
- spdlog only; EveryFrame backlog warning is rate-limited to the stats poll.

Follow-ups:
- Hardware acceptance runs on real EGrabber + MindVision rigs per the runbook.
- Frame-age display in diagnostics UI beyond the badge (issue #333 presentation
  details).
- Verify `CameraGetImageBufferPriority` signature against the exact SDK build
  shipped on production machines before enabling MIB_HAS_MINDVISION there.
