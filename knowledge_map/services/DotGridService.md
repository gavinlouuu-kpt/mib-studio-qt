# DotGridService

> Low-rate wafer localization: decodes the dot-grid fiducial pattern in the
> latest live frame and publishes where on the wafer (and on which chip) the
> camera is looking.

**Source:** `src/backend/services/DotGridService.cpp`,
`include/backend/services/DotGridService.h`; decoder/codebook in
`src/backend/processing/DotGridDecoder.cpp`, `DotGridCodebook.cpp`
(`mib_processing`, Qt-free)
**Related:** [[PlaybackService]], [[../data-model/FrameStore]],
[[../architecture/AppBackend]], [[../frontend/System-Utilities]] (PlaybackPanel
overlay), ADR 0006, `docs/architecture/dot-grid-localization.md`,
`docs/howto/dot-grid-mask-generation.md`

## Responsibility

- Hold the `Codebook` (pattern definition) built from `Config::codebook`
  (seed + geometry) or loaded from `Config::codebookPath` (a `codebook.json`
  from `scripts/dot_grid`, which also carries the chip table). `setConfig()`
  rebuilds it only when the source changed; invalid parameters or a missing
  file are rejected (`false` + error string) and the previous codebook stays.
- Every `intervalMs` (default 250) read the latest **committed** FrameStore
  frame (`committedCount()` / `latestCommittedIndex()` / `getByWriteIndex()`),
  skipping intermediate frames, convert it to 8-bit grey (`frameToGray`,
  16-bit containers normalised), run `dotgrid::Decoder::decode`, and publish
  a `Pose` (valid or not, always with `reason`, `frameIndex`, `timestampNs`,
  image size, detected dots for the overlay).
- `getLatestPose()` snapshot under `poseMutex_`; `setPoseCallback()` invoked
  on the service thread after the snapshot is updated. `decodeImage()` runs
  the decoder synchronously on any image (UI / tests).
- Metrics: `decodeAttempts()`, `decodeSuccesses()`, `lastDecodeMs()`;
  `isEnabled()` is an atomic mirror of `Config::enabled` for the UI tick.

## Threading

- One `std::thread` (`loop()`), `running_` atomic, wake-up through
  `wakeCv_` on `stop()`/`setConfig()`. `start()`/`stop()` idempotent.
- Reads only public FrameStore APIs; one frame copy per sample. Never runs
  on the capture, realtime or GUI thread. Decodes a frame index at most once.
- Config under `configMutex_`; the decoder is a `shared_ptr<const>` swapped
  atomically under that mutex, so a config change never races a decode.

## Configuration

`config.json` → `dot_grid` (applied live by `AppConfigWatcher`): `enabled`,
`interval_ms`, `um_per_px_hint` (0 → `pixel_to_micron_factor`), `min_votes`,
`min_agreement`, `codebook_path`, `codebook{seed, columns, rows, pitch_um,
dot_diameter_um, displacement_um, origin_x_um, origin_y_um}`.
`MIB_DISABLED_SERVICES=dot_grid` leaves the service constructed but not
started. The Preview page's **Wafer Grid** button toggles `enabled`.

## Manual & periodic test paths

- `ctest -R dot_grid`: `processing.dot_grid_codebook` (C++/Python golden
  parity), `processing.dot_grid_decoder` (rotation, mirror, channel band,
  noise, dropouts, blank/noise/16-bit), `backend.dot_grid_service`
  (lifecycle, sampling under a producer burst, disable/enable, bad config,
  stop idempotence, watchdog), `scripts.dot_grid_reference`.
- Mock camera: `python3 scripts/dot_grid/dotgrid_cli.py mockdir …` then
  `MIB_CAMERA_MODE=mock` (see the how-to).

## Gotchas

- The decoder reports **mask** coordinates; the Wafer_soRT design is 1.5 %
  pre-enlarged. `umPerPx` is measured from the image, the hint only sizes
  the blob detector.
- Mask and app must share seed/pitch/dot/shift/origin; a larger
  `columns`/`rows` is a superset (prefix-stable delta sequences), a smaller
  one silently truncates the wafer.
- A view without enough dots (inside a 1 mm port, or a 20x view centred on
  a channel with pitch > 30 µm) yields `reason = "no consistent code
  window"`; the pose is never left stale-valid.
- `frameToGray` trusts `linePitch`; a frame with `linePitch == 0` is treated
  as tightly packed.
