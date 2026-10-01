# AravisCamera

`AravisCamera` is the optional, Qt-free `ICamera` consumer in
`src/backend/camera/aravis/`. It is enabled with `MIB_ENABLE_ARAVIS=ON` and
requires the pinned `aravis-0.10` package described by `env/aravis.toml`.
The default build remains dependency-free.

## First-slice contract

- Device selection is explicit through `MIB_ARAVIS_DEVICE_ID`; the Aravis Fake
  interface is enabled only when `MIB_ARAVIS_FAKE=true|1|yes`.
- The adapter delivers copied, single-part Mono8 images in `EveryFrame` order.
  `LatestFrame`, multipart payloads and non-Mono8 buffers are rejected with
  structured `aravis.*` failures.
- SDK buffers are returned on every completed-buffer path, and delivered bytes
  remain owned by `camera::common::Frame` after buffer reuse.
- Optional SFNC software-trigger mode is available through the adapter options;
  a receive timeout while acquisition remains running is a non-delivery and
  does not alter the last copied frame.
- Aravis timestamps remain opaque device ticks (`Unsupported` validity) until a
  producer-specific rate/host mapping is verified. They are never advertised
  as host-comparable.
- `stop()` uses a bounded stream pop and serialized SDK-resource ownership;
  adapter destruction does not call process-global `arv_shutdown()`.

The Fake consumer validates the Aravis side of the future
`PL -> Linux driver -> GenTL producer` path. It does not validate PZ7035
transport, SSD recording, or the future GenTL producer.

**Related:** [[ICamera]], [[../services/CaptureService]], [[../architecture/AppBackend]]
