## 2026-10-07 — Six develop fixes and regression coverage (#405)

- #405: [[frontend/NanopositionerTab]] gates retained status callbacks against
  destruction; callback tests cover worker delivery, replacement and shutdown.
- TD-21: observe-only connections and unrelated saves preserve configured
  initial voltage instead of replacing it with a live/zero reading.
- #394 / PR #396: Contract-1 pixel-exact kernel goldens cover dark suppression,
  threshold equality, missing background, ROI clipping and borrowed inputs.
  See [[services/ProcessingService]].
- #226 / PR #212: [[services/Hdf5Service]] chunks one frame or one series
  member per write; batched round-trip and layout assertions cover the change.
- #230 / PR #231: [[frontend/HdfReviewTab]] and [[services/HdfExportService]]
  append Young's modulus (kPa), preserving column order and leaving NaN blank.
- Windows full installer checks all three required x64 VC++ runtime DLLs in
  native System32; updater has no redist check. See [[build-and-run/Build]].

No bridge ABI or processing science changes. Tests run offline with fake
nanopositioner drivers; Windows installation and TSan require their own lanes.
