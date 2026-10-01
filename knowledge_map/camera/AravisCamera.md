# AravisCamera

`AravisCamera` is the optional, Qt-free `ICamera` consumer in
`src/backend/camera/aravis/`. It is enabled with `MIB_ENABLE_ARAVIS=ON` and
requires the pinned `aravis-0.10` package described by `env/aravis.toml`.
The default build remains dependency-free.

## Contract

- Device selection: `MIB_ARAVIS_DEVICE_ID` when set; otherwise the first
  device whose vendor is `AravisCameraOptions::preferredVendor` (`YOFO`, the
  PZ7035 GenTL producer), else the first physical device. The Aravis Fake
  interface is enabled only when `MIB_ARAVIS_FAKE=true|1|yes`.
- The GigE Vision interface is disabled unless `MIB_ARAVIS_GIGE=true|1|yes`:
  its discovery resolves device names with `getaddrinfo`, which costs seconds
  per open on the isolated PZ7035 network.
- Optional `region`, `frameRateHz` and `exposureUs` options are applied at
  start in that order (the maxima depend on the region) and read back into
  `sessionInfo()`. A region the device does not apply exactly fails start
  (`aravis.region_rejected`); frame rate and exposure are clamped by the
  device and reported as `frameRateClamped` / `exposureClamped`.
- `sessionInfo()` also carries the PZ7035 rate model when the device has it:
  `PzBandCount`, `PzDeliveredFrameRate`, `PzDeliveredFrameRateLimit` and
  `PzFrameRateLimitReason`. `AcquisitionFrameRate` is the sensor rate; a
  banded full-field (Overview) image arrives at the delivered rate, and the
  UI must show both (YOFO Studio ADR 0008).
- The adapter delivers copied, single-part Mono8 images. `EveryFrame` keeps
  order; `LatestFrame` (preview) hands older completed buffers back to the
  producer and counts them in `intentionallyDiscardedFrames`. Multipart
  payloads and non-Mono8 buffers are rejected with structured `aravis.*`
  failures.
- SDK buffers are returned on every completed-buffer path, and delivered bytes
  remain owned by `camera::common::Frame` after buffer reuse.
- Optional SFNC software-trigger mode is available through the adapter options;
  a receive timeout while acquisition remains running is a non-delivery and
  does not alter the last copied frame.
- Timestamps: the YOFO producer stamps buffers with `CLOCK_MONOTONIC` in this
  process when the PL has delivered the image, so they are declared
  `HostSteadyNs` / `TransportReceipt` / `Valid` on Linux. Other devices keep
  opaque device ticks (`Unsupported`). Neither is advertised as comparable to
  `Tools::getTimestamp()`.
- `stop()` uses a bounded stream pop and serialized SDK-resource ownership;
  adapter destruction does not call process-global `arv_shutdown()`.

The Fake consumer validates the Aravis side. The PZ7035 producer
(`pz7035-imx426` `gentl/`) runs on x86 as a pattern device
(`PZ_GENTL_PATTERN=1`, `GENICAM_GENTL64_PATH` pointing at its `.cti`) and on
the PS against the PL over UIO.

**Related:** [[ICamera]], [[../services/CaptureService]], [[../architecture/AppBackend]]
