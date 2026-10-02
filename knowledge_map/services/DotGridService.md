# DotGridService

> Low-rate wafer localization: decodes the dot-grid fiducial pattern in the
> latest live frame and publishes where on the wafer the camera is looking,
> which registered chip **design** it is, and which chip (die) of the wafer.

**Source:** `src/backend/services/DotGridService.cpp`,
`include/backend/services/DotGridService.h`; decoder/codebook in
`src/backend/processing/DotGridDecoder.cpp`, `DotGridCodebook.cpp`,
`DotGridRegistry.cpp` (`mib_processing`, Qt-free); bundled registry
`resources/defaults/dot_grid/registry.json`
**Related:** [[PlaybackService]], [[../data-model/FrameStore]],
[[../architecture/AppBackend]], [[../frontend/System-Utilities]] (PlaybackPanel
overlay), ADR 0008, ADR 0009 (design registry),
`docs/architecture/dot-grid-localization.md`,
`docs/howto/dot-grid-mask-generation.md`

## Responsibility

- Hold the set of designs to decode against as a `dotgrid::Registry`
  (`activeRegistry()`). Source, first match wins: `Config::codebookPath` (one
  `codebook.json`, explicit override) → `Config::registry` when non-empty
  (every registered design; the frontend passes the bundled registry merged
  with `registry_path`) → one codebook generated from `Config::codebook`.
  `setConfig()` rebuilds the decoder only when the source changed (registry
  compared by `fingerprint()`); invalid parameters or a missing file are
  rejected (`false` + error string) and the previous decoder stays.
- The decoder tries every design (detection + lattice fit once per dot
  geometry, vote + verification per design); `Pose::designId` /
  `designName` name the design that decoded (empty in single-codebook
  mode), `Pose::chip` the die from that design's chip table.
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
`min_agreement`, `registry_path` (local registry merged over the bundled
one; relative → config dir), `codebook_path`, `codebook{seed, columns, rows,
pitch_um, dot_diameter_um, displacement_um, origin_x_um, origin_y_um}`.
`AppConfigWatcher::loadDotGridRegistry` reads `:/defaults/dot_grid_registry.json`
and merges the local file; clashes are logged and skipped.
`MIB_DISABLED_SERVICES=dot_grid` leaves the service constructed but not
started. The [[../frontend/OverviewTab]]'s **Wafer Grid** button toggles
`enabled`; the Experiment tab has no toggle and never decodes.

## Codec cores (ADR 0010)

The service decodes through `dotgrid::DesignDecoder` built from
`codecs_` (`CodecSet::bundled()`: the contract-1 core `mseq63-delta2`,
version `MIB_DOTGRID_CORE_VERSION` from `scripts/dot_grid/dotgrid/VERSION`)
and the active registry. Each design goes only to the core of its
`codecContract`; designs whose contract has no core are logged at
`setConfig()` as unsupported and never decoded. Every `Pose` carries
`codecContract`, `coreVersion`, `coreSource`; `activeCodecs()` lists the
cores. `codecs_` is fixed at construction in phase 1 (plugin cores come with
the loader, exec plan `2026-10-02-dot-grid-codec-cores`). Gold:
`processing.dot_grid_codec_gold`.

## Pause (Overview only)

`setPaused(bool)` / `isPaused()` is a runtime gate on top of `enabled`,
not persisted and off by default (headless users and tests decode whenever
enabled). `OverviewTab` pauses in its constructor and `hideEvent` and
resumes in `showEvent`, so the service decodes only while the Overview is on
screen. `setPaused(false)` sets `wakeRequested_` under `wakeMutex_` so the
loop decodes the newest frame immediately instead of after `interval_ms`
(`setConfig()` and `stop()` use the same `wake()`).

## Manual & periodic test paths

- `ctest -R dot_grid`: `processing.dot_grid_codebook` (C++/Python golden
  parity), `processing.dot_grid_decoder` (rotation, mirror, channel band,
  noise, dropouts, blank/noise/16-bit), `processing.dot_grid_registry`
  (parse/reject rules, merge, design attribution across two geometries,
  unregistered design never decodes, bundled registry regenerates the
  archived Wafer_soRT codebook), `backend.dot_grid_service` (lifecycle,
  sampling under a producer burst, disable/enable, bad config, registry mode
  and source precedence, stop idempotence, watchdog),
  `scripts.dot_grid_reference` (incl. `register` end to end and the Python
  codec gold), `processing.dot_grid_codec_gold` (C++ core vs
  `scripts/dot_grid/gold/codec-contract1.json`).
- Registry: `python3 scripts/dot_grid/dotgrid_cli.py list | check --cross-check`.
- Mock camera: `python3 scripts/dot_grid/dotgrid_cli.py mockdir …` then
  `MIB_CAMERA_MODE=mock` (see the how-to).

## Gotchas

- The decoder reports **mask** coordinates; the Wafer_soRT design is 1.5 %
  pre-enlarged. `umPerPx` is measured from the image, the hint only sizes
  the blob detector.
- A design's codec contract is permanent (its mask is): never edit a
  contract's encode gold; a new encoding is a new contract + core + gold file.
- A design's seed is its identity: never change or reuse a registered
  seed or geometry; a changed layout is a new id. Two designs decoding the
  same frame is reported as `ambiguous design (...)`, never as a pose.
- Each distinct dot geometry in the registry adds a detection pass per
  frame (~2–3× decode time); same-geometry designs are free.
- Mask and app must share seed/pitch/dot/shift/origin; a larger
  `columns`/`rows` is a superset (prefix-stable delta sequences), a smaller
  one silently truncates the wafer.
- A view without enough dots (inside a 1 mm port, or a 20x view centred on
  a channel with pitch > 30 µm) yields `reason = "no consistent code
  window"`; the pose is never left stale-valid.
- `frameToGray` trusts `linePitch`; a frame with `linePitch == 0` is treated
  as tightly packed.
