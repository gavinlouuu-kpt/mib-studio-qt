# EGrabberCamera

> Production [[ICamera]] implementation backed by the Euresys EGrabber SDK.

**Source:** `src/camera/common/EGrabberCamera.cpp`,
`include/camera/common/EGrabberCamera.h`
**Related:** [[ICamera]], [[../services/CaptureService]],
[[../services/CameraControlService]]

## Responsibility

- Wrap an `Euresys::EGrabber` handle scoped to a specific
  `(interfaceIndex, deviceIndex)` selection.
- Implement `start`, `stop`, `grabFrame`, `pollStats`.
- Implement the delivery-mode contract (`deliveryCapabilities`,
  `activeDeliveryMode`, `pollAcquisitionQueueStats`): both `EveryFrame` and
  `LatestFrame` are supported, mode changes require a restart, and Coaxlink
  buffer timestamps are host-comparable on Windows only (µs since computer
  startup, same domain as `Tools::getTimestamp` QPC µs).
- Expose `configureTriggerOutput` + `setTriggerOutput` for
  [[../services/TriggerService]].
- Surface `checkDeviceHealth()` to detect camera disappearance.
- On non-Windows builds (`MIB_HAS_EGRABBER=0`), compile as a safe stub that
  logs unsupported hardware start and returns `false`/no-op for camera I/O.

## Key reference material

- **Euresys SDK samples** — `egrabber-sample-programs/` in this repo
  (shipped as a reference source tree). Always check for a ready-made
  pattern before rolling your own SDK call. See
  [[../conventions/Code-Conventions]].
- **Integration note** — `docs/integration/egrabber.md`.
- **Start/stop safety** — `docs/howto/safe-start-stop-egrabber.md` and
  task `knowledge_map/task/2025-11-14-safe-start-stop-egrabber.md`.
- **Device reset flow** — task `knowledge_map/task/camera-reset.md`.

## Delivery modes (issue #331)

- **Start sequencing:** `start()` calls `grabber_->start()` once and lets the
  SDK do the ordering — it starts the data stream first, then executes
  `AcquisitionStart` on the remote device. Never send a manual
  `AcquisitionStart` before `grabber_->start()`: remote acquisition must be
  started exactly once, and only after the data stream is ready. The
  defensive `AcquisitionStop`-before-start block stays, and `stop()` remains
  symmetric (remote `AcquisitionStop`, then `grabber_->stop()`).
- **EveryFrame** keeps the plain FIFO pop: every completed buffer is copied
  and delivered in acquisition order.
- **LatestFrame** drains stale buffers in `replenishPendingFrames()` *before*
  the expensive copy: while `getPendingEventCount<NewBufferData>() > 1`, pop
  a `ScopedBuffer` and let it destruct immediately (requeues to the input
  FIFO), counting each drop in the atomic `intentionallyDiscardedFrames_`;
  then pop and copy the final (freshest) buffer. Never
  `flushEvent<NewBufferData>()` — it would discard events without releasing
  the buffers they own.
- **BufferPartCount=1 in LatestFrame:** forced at `start()` regardless of
  `config_.bufferPartCount` (warn if the config asked for more) — multi-frame
  batching per buffer would add exactly the latency the mode exists to avoid.
- **`activeDeliveryMode()`** reports the mode confirmed at the most recent
  successful `start()` (`confirmedDeliveryMode_`); before any start it falls
  back to `config_.deliveryMode`.
- **`pollAcquisitionQueueStats()`** maps GenTL stream counters:
  `STREAM_INFO_NUM_AWAIT_DELIVERY` → completed queue depth,
  `STREAM_INFO_NUM_QUEUED` → input buffer count, `STREAM_INFO_NUM_UNDERRUN` →
  underruns, `STREAM_INFO_NUM_DELIVERED` → delivered frames, plus the
  intentional-discard counter (reset at `start()`). No documented counter
  maps to transport loss, so `transportLossValid` stays false.

## Timestamps (issue #368)

`Frame::timestamp` is the Coaxlink `BUFFER_INFO_TIMESTAMP` / per-part custom
timestamp: microseconds since host boot. `timestampDescriptor()` reports
`hostMonotonicUs @1e6 Hz, transportReceipt, valid` on Windows (QPC domain =
`Tools::getTimestamp()`, hence `timestampsHostComparable`), and
`unknown/unsupported` on other platforms where the mapping is unverified. It
marks transport receipt, not exposure.

## Gotchas

- **StreamModule counters** must be refreshed before stopping capture or
  the final `frameRate` / `dataRate` read will be zero. See
  `fps_mbs_zero.md` task and [[../conventions/Code-Conventions]].
- GenICam `DeviceReset` is best-effort; EGrabber will temporarily lose the
  device handle — [[../services/CameraControlService]] handles this.
- Pixel format mapping: EGrabber PFNC codes flow through unchanged in
  `Frame::pixelFormat`.
- Header and source are guarded by `MIB_HAS_EGRABBER`; non-Windows compile must
  not include `EGrabber.h`.
- **Delivered-buffer validation:** `replenishPendingFrames` rejects a buffer
  (warn + return) when the SDK reports a null base pointer, a zero part size,
  or a part size smaller than the strided geometry consumers will trust
  (`(height-1)*pitch + width` bytes) — on unplug/partial delivery the copy
  would otherwise segfault or a garbage size would `bad_alloc`.
- **Trigger-thread lifetime protocol:** `setTriggerOutput` runs on the
  [[../services/TriggerService]] thread and takes `triggerMutex_` (never
  `stateMutex_`, which `stop()` holds across ~360 ms of teardown sleeps).
  Every assignment/reset of `grabber_` also holds `triggerMutex_`, so a
  trigger pulse racing a camera stop fails cleanly instead of dereferencing a
  destroyed grabber. `running_` is `std::atomic<bool>` for the same reason
  (read lock-free by the trigger thread and `isRunning()`).
- **One register write per pulse edge (issue #227):** `LineSelector` is
  GenApi nodemap state owned by this `grabber_` instance, so
  `setTriggerOutput` selects the line once (tracked by `triggerLineApplied_`,
  reset on `start()` / `configureTriggerOutput()` / any write failure) and
  then writes only `LineSource` per edge — halving the PCIe control
  transactions per pulse. The profile config script (which also touches
  `LineSelector`) runs through a **separate** short-lived `EGrabber` handle
  in [[../services/CameraControlService]] `applyScriptToDevice`, so it
  cannot disturb this instance's selection; a fresh `start()` re-selects
  anyway. Do not add other `LineSelector` writes on *this* grabber handle
  without clearing `triggerLineApplied_`.
