# Qt → React/Tauri migration: backend de-Qt slice 2 (mock-camera decode)

Date: 2026-07-15
Epic: #246 · ADR: `docs/decisions/0001-react-tauri-migration.md` ·
Plan: `docs/exec-plans/active/2026-07-15-qt-decoupling-and-tauri-migration.md`

## Context

Second backend de-Qt slice. `MockCamera` was the **only** backend user of
`QImage`/`QImageReader` (a `Hdf5Service.cpp` grep hit is a stale comment), so
removing it lets `Qt6::Gui` drop from the backend entirely. Chosen ahead of the
serial abstraction because it is small, self-contained, and fully
Linux-verifiable by the existing `camera.mock_smoke` test.

## What shipped

- `src/backend/camera/mock/MockCamera.cpp` — `loadFrameFromPath` rewritten to
  decode every supported extension (PNG/JPEG/BMP/TIFF) with
  `cv::imread(path, cv::IMREAD_GRAYSCALE)`, generalizing the OpenCV block that
  previously ran only as a TIFF fallback. Output stays PFNC Mono8
  (`0x01080001`); rows are packed tightly (`linePitch == width`, non-continuous
  `cv::Mat` copied row-by-row via `step[0]`). Deleted the `QImageReader` path,
  the `toQString` helper, and the `<QImage>`/`<QImageReader>`/`<QString>`
  includes. The header was already Qt-free.
  - Intentional deltas: `linePitch` is now always `width` (was Qt's padded
    `bytesPerLine()`; downstream tolerates pitch ≥ width); QImage EXIF
    auto-transform dropped (irrelevant for synthetic mock frames).
- `src/backend/CMakeLists.txt` — removed `Qt6::Gui` from the `mib_backend`
  link.
- `cmake/MIBDependencies.cmake` — moved `Gui` out of the base Qt component set
  into the frontend-only list, so `MIB_BUILD_BACKEND_ONLY` no longer requires
  Qt Gui. Verified via repo-wide grep that no backend target/source uses a
  `Qt6::Gui` symbol.
- `tests/camera/mock_camera_smoke_test.cpp` — added a `frameIsUniform` helper
  asserting Mono8 format, tight packing, and exact pixel values; the two PNG
  frames must decode to their written fills (60, 120), plus a new TIFF sub-test
  (value 200) exercising the `.tif` path through the unified decode.

## Verification

- Built `MockCamera.cpp` + `mock_camera_smoke_test.cpp` standalone against
  system OpenCV + spdlog and ran it — passes (shape, pixel values, ordering,
  stop-at-end, TIFF decode).
- `check_docs.py` / `check_screenshots.py` pass. The full `linux-backend-only`
  configure/build (which confirms the `Qt6::Gui` link/`find_package` change)
  runs in the `backend-ci` lane; the Qt SDK is not fully present in the dev
  container.

## Not done here (tracked in the exec-plan)

Serial abstraction (`ISerialPort`, drops `Qt6::SerialPort`); LUT-catalog
`QtNetwork`/paths seam; crash-reporter `QString` glue; then the final
`Qt6::Core`/`AUTOMOC` drop (Phase 1 exit gate).
