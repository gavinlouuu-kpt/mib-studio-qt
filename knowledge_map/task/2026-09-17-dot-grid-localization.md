Title: Dot-grid wafer localization (fiducial pattern + decoder + overlay)

Context:
- The camera views a 562 × 351 µm patch of the Wafer_soRT wafer at 20x
  through the glass and cannot tell where it is. An Anoto-style dot lattice
  fabricated in the channel layer makes every ~6 × 6 dot patch unique.
- Design source: YOFO drive, `Wafer_soRT-DC chip_channel layer_height 30 um_1.5% enlarged.dwg`
  (AutoCAD 2018; LibreDWG ≥ 0.14 `dwg2dxf` reads it). 100 mm wafer, 20 chips
  of 24.4 × 7.2 mm, 30 µm channels.

Implementation Notes:
- `backend::dotgrid::Codebook` / `Decoder` in `mib_processing`
  (`src/backend/processing/DotGrid*.cpp`), Qt-free; synthetic renderer for tests.
- `backend::services::DotGridService`: one thread, samples latest committed
  FrameStore frame every `interval_ms`, publishes `Pose` snapshot + callback.
  Constructed in `AppBackend::initialize()` after the FrameStore, started
  unless `MIB_DISABLED_SERVICES` contains `dot_grid`, stopped in `shutdown()`
  after the trigger service.
- `PlaybackPanel`: **Wafer Grid** button, `DotGridOverlay` drawn by
  `ImageCanvas` (dots, centre cross, pose text); follows the service's
  `isEnabled()` so config-driven enables update the button.
- `AppConfigWatcher` applies `dot_grid` from `config.json`
  (defaults added in `resources/defaults/config.json`).
- `scripts/dot_grid/`: Python reference (codebook/decoder/renderer), DXF →
  GDS/DXF/CSV generator, mock-frame writer, unittest suite (also CTest
  `scripts.dot_grid_reference`).
- Chosen mask parameters: pitch 30 µm, dot 12 µm, shift 5 µm, seed 7,
  keep-out 50/200/300 µm, 3 mm wafer edge → 6.4 M dots. Shipped codebook:
  `resources/defaults/dot_grid/wafer_soRT_2025-03-16_seed7_p30.json`.

Verification:
- `ctest --preset linux-backend-only-test -R dot_grid` (4 tests) green;
  synthetic benchmark in `docs/architecture/dot-grid-localization.md`.

Follow-ups:
- Real-frame validation once the mask is fabricated; HDF5 pose persistence;
  CAD overlay; exec plan `docs/exec-plans/active/2026-09-17-dot-grid-localization.md`.
