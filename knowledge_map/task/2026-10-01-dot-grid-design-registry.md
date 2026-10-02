Title: Dot-grid design registry (which chip design is under the objective)

Context:
- Follow-up to [[2026-09-17-dot-grid-localization]]: more chip designs will
  carry a dot grid, and the app has to tell them apart from the frame alone.
  Developers need one way to add a design and get its mask layer.
- Synthetic finding that made it simple: a frame of one seed never decoded
  under another seed (0/120 at 4x–20x), so the seed is the design identity.
  ADR 0009.

Implementation Notes:
- `resources/defaults/dot_grid/registry.json` (schema v1): per design id,
  name, revision, status, seed, lattice, geometry, origin, chips, keep-outs,
  source DXF sha256. Parameters only; codebooks are regenerated from the
  seed. Wafer_soRT is `wafer-sort-rt`, seed 7, and regenerates the archived
  `wafer_soRT_2025-03-16_seed7_p30.json` bit for bit.
- Python: `scripts/dot_grid/dotgrid/registry.py` (`Registry`, `Design`,
  `decode_registry`, `cross_check`); CLI `register`, `mask`, `list`, `check`;
  `decode IMAGE` now tries the whole registry (`--codebook` restricts);
  `render`/`mockdir` accept a design id. `register` into a local file also
  avoids bundled ids/seeds and cross-checks against bundled designs.
- C++: `backend::dotgrid::Registry` (`DotGridRegistry.{h,cpp}`, Qt-free,
  nlohmann JSON); `Decoder(shared_ptr<const Registry>)` groups designs by dot
  geometry, shares detection + lattice fit + line phases, votes per design;
  exactly one hit or `ambiguous design (...)`. `DecodeResult`/`Pose` gained
  `designId`, `designName`, `designsTried`. `Codebook::generate` takes a
  design name.
- `DotGridService::Config::registry`; precedence codebook_path → registry →
  params; decoder rebuilt only when the registry fingerprint changes.
- Frontend: registry compiled into `defaults.qrc` as
  `:/defaults/dot_grid_registry.json`; `AppConfigWatcher` merges
  `dot_grid.registry_path`; overlay prints design name + chip.

Verification:
- `ctest -R dot_grid` (5 tests) green on the Linux backend preset, plus
  `mib_studio_qt` built on `linux-system-release`.
- Timing (cloud container, Release): 1/2/4/8 same-geometry designs all decode
  in 120–160 ms; a second dot geometry gives 360–420 ms.

- Sanitizers: the dot-grid tests now pass the TSan lane. OpenCV's TBB
  `parallel_for_` produced false races (worker alloc vs caller free); new
  `tests/support/opencv_tsan.h` serializes OpenCV under TSan only. The
  service burst check became a ratio gate (burst time < half of decoding
  every frame) instead of "> 20 frames in 150 ms".
- Real app: `screenshot_tour` on Wafer_soRT mock frames shows the overlay
  "Wafer_soRT DC sorting chip (30 um channels)   chip R4C2" with the pose.

- 2026-10-02 follow-up (review): localization belongs to the Overview tab.
  Toggle + overlay moved from `PlaybackPanel` to `OverviewTab` /
  `SimpleImageCanvas`; `DotGridService::setPaused` driven by the Overview's
  show/hide events; `wake()` + `wakeRequested_` so resume and config changes
  end the interval wait (the old bare `notify_all` was ignored by the
  `wait_for` predicate). Test `frontend.dot_grid_overview`.

Follow-ups:
- In-app "add design" dialog; persist `designId` with the pose in HDF5.
