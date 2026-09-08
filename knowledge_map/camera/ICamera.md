# ICamera (interface)

> Abstract base for any frame source. Deliberately small; normalizes
> hardware vs mock differences so [[../services/CaptureService]] doesn't
> care.

**Source:** `include/backend/camera/common/ICamera.h`, `include/backend/camera/common/Frame.h`

## Types

- `camera::common::Frame` — `{ width, height, pixelFormat (PFNC),
  linePitch, timestamp, data }`.
- `CameraConfig` — `{ bufferPartCount = 1, numBuffers = 20, deliveryMode }`.
- `CameraStats` — `{ frameRate, dataRateMBps }`.
- `FrameDeliveryMode` — `EveryFrame` (ordered, never intentionally skips)
  vs `LatestFrame` (drains stale completed SDK buffers before the copy,
  returns the freshest frame, counts every deliberate discard). Serialized
  as `"everyFrame"` / `"latestFrame"` via `toString` /
  `frameDeliveryModeFromString`; unknown text maps to `EveryFrame` so
  legacy profiles keep ordered behavior.
- `FrameDeliveryCapabilities` — per-backend support flags
  (`supportsEveryFrame`, `supportsLatestFrame`,
  `modeChangeRequiresRestart`, `timestampsHostComparable`).
- `AcquisitionQueueStats` — queue telemetry with *distinct* counters for
  intentional discards, transport loss, and underruns (never merged), plus
  completed-queue depth and input-buffer count. Fields a backend cannot
  observe keep their `*Valid` flag false ("unknown", not "zero").
- `CameraFailure` — `{ code, message }` structured reason for the most
  recent start rejection / stream fault (issues #365/#366). `code` is a
  stable token (e.g. `mindvision.isp_format_rejected`,
  `mindvision.frame.frameGeometryMismatch`); [[../services/CaptureService]]
  copies it into its lifecycle snapshot so the UI/preflight can show *why*
  instead of a generic "failed to start".

## Virtual methods

```cpp
virtual void applyConfig(const CameraConfig& config) = 0;
virtual bool start() = 0;
virtual void stop()  = 0;
virtual bool isRunning() const = 0;

virtual bool grabFrame(Frame& out) = 0;          // blocks; false on stop
virtual bool pollStats(CameraStats& out) const = 0;

virtual bool checkDeviceHealth() const { return true; }
virtual void configureTriggerOutput(const std::string& lineSelector) {}
virtual bool setTriggerOutput(bool high) { return false; }   // SORT pulse out
virtual bool softTrigger() { return false; }  // software ACQUISITION trigger
virtual CameraFailure lastFailure() const { return {}; } // structured start/stream fault
virtual TimestampDescriptor timestampDescriptor() const { return {}; } // Frame::timestamp semantics (issue #368)

// Delivery-mode contract (defaults: EveryFrame-only, no queue telemetry)
virtual FrameDeliveryCapabilities deliveryCapabilities() const;
virtual FrameDeliveryMode activeDeliveryMode() const;   // backend-confirmed
virtual bool pollAcquisitionQueueStats(AcquisitionQueueStats& out) const;
```

## Related

- [[EGrabberCamera]] — hardware implementation
- [[MindVisionCamera]] — MindVision SDK implementation (only backend
  overriding `softTrigger`)
- [[MockCamera]] — folder-backed implementation
- [[../services/TriggerService]] calls `setTriggerOutput` via the live
  camera pointer (sort pulse); [[../services/CaptureService]] exposes
  `softTriggerActiveCamera()` for the acquisition trigger — the two are
  opposite signal directions and must not be conflated
- [[../services/CameraControlService]] uses the Euresys SDK directly for
  discovery; it does not go through `ICamera`.

## Timestamp contract (issue #368)

`Frame::timestamp` is **not** assumed to be nanoseconds. Each backend
declares `timestampDescriptor()` (`TimestampValue.h`): clock domain
(`deviceTicks` / `hostMonotonicUs` / `hostSteadyNs` / `hostWallClockNs` /
`unknown`), ticks per second of the stored value after the adapter's
documented normalization, the native counter rate, the semantic
(`deviceCapture`, `transportReceipt`, `hostReceipt`, `synthetic`, ...),
validity, counter width, and session generation. `Frame::rawDeviceTicks`
keeps the native counter for audit. `TimestampValue::toNanoseconds()` is the
only conversion boundary (checked, empty on overflow or undeclared units) and
`differenceNs(a, b)` refuses to subtract across clock domains or across
device-tick sessions. `Frame::hostTimestampUs` (host monotonic receipt) is a
separate value on a separate clock and is never mixed with the device stamp.

| Backend | domain | ticks/s stored | native | semantic |
|---|---|---|---|---|
| [[MindVisionCamera]] | deviceTicks | 1e9 (ns) | 10 000 (`uiTimeStamp`, 0.1 ms, 32-bit) | deviceCapture |
| [[EGrabberCamera]] | hostMonotonicUs (Windows; `unknown`/unsupported elsewhere) | 1e6 | — | transportReceipt |
| [[MockCamera]] | hostSteadyNs | 1e9 | — | synthetic |
| default / undeclared | unknown | 0 | — | unsupported |

## Gotchas

- `pixelFormat` uses Euresys PFNC codes (see [[../domain/Glossary]]).
- `linePitch` may exceed `width` (stride padding) — always honor it when
  iterating frame data.
