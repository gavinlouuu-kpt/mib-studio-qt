# Qt decoupling and React + Tauri migration

Status: active (2026-07-15) — **Phases 1–3 landed; Phase 4 underway.** The C++ backend is fully
Qt-free and `MIB_BUILD_BACKEND_ONLY` configures/builds/tests with **no Qt SDK**
(verified by uninstalling Qt6 locally; enforced by `backend-ci.yml`). Phase 2
delivered the Rust ↔ C++ bridge (`crates/mib-bridge`, `cxx` over `BackendFacade`,
ADR 0003) with a headless `cargo test` contract lane (`bridge-ci.yml`). Phase 3
delivered the first React + Tauri v2 vertical slice (`desktop/`, mock camera end
to end) — headless-tested via `cargo test` + an Xvfb GUI smoke
(`desktop-ci.yml`). Phase 4 slices 1–2 landed the bridge
schema-v2 review commands (record-to-HDF5 / load-and-scrub UI) and schema-v3
processing commands (realtime toggle + pixel→micron + live fps overlay).

Tracks epic #246. The platform decision is recorded in ADR
[`../../decisions/0001-react-tauri-migration.md`](../../decisions/0001-react-tauri-migration.md).
This plan is the living breakdown that would otherwise be GitHub sub-issues:
the Qt inventory, the feature-parity matrix, the performance budgets, and the
incremental PR order.

## Goal

Ship one supported MIB Studio desktop app on React + Tauri v2 with a **Qt-free
C++ backend** and a Qt-free backend build/test lane, preserving every operator
workflow in [`../../manual/`](../../manual/), data compatibility, reliability,
and performance. Migrate in vertical slices from current `main`; keep the Qt
shell building until Tauri passes the epic's exit gate.

## Principles (from ADR 0001)

1. Start from `main`; do not merge PR #59 wholesale.
2. Every slice is usable, tested, and benchmarked end to end.
3. Backend contracts carry no Qt types; the UI-neutral seam is
   `backend::bridge::BackendFacade`.
4. Protect the frame hot path — a per-frame PNG/base64 transport is not the
   production design unless it meets the budgets below.
5. Preserve HDF5 files, profiles, config, update channels, logs, and crash
   reporting, or migrate through tested tooling.

## Qt dependency inventory (backend)

No `Q_OBJECT`/moc in backend headers; threading is already std. Qt survives in
these clusters. "Contract" = Qt appears in a public header.

| # | Cluster | Files | Qt used | Contract? | Replacement | Status |
|---|---------|-------|---------|-----------|-------------|--------|
| 1 | Modbus framing | `include/backend/services/ModbusRtu.h` | `QByteArray` | yes | `std::vector<uint8_t>` | **done (this PR)** |
| 2 | MindVision JSON | `include/backend/camera/mindvision/MindVisionConfig.h` (+ `MindVisionCamera.cpp`, `CameraControlService.cpp` reads) | Qt JSON, `QFile` | yes | `nlohmann_json` + `std::ifstream` | **done (this PR)** |
| 3 | Syringe-pump serial I/O | `src/backend/services/SyringePumpService.cpp` (+ new `ISerialPort.h`, `SerialPort{Posix,Win32}.cpp`) | `QSerialPort` | now Qt-free | `ISerialPort` interface + factory + POSIX/Win32 impls | **done (slice 3)** — `Qt6::SerialPort` dropped; fake-serial + pty-loopback tests |
| 4 | LUT catalog | `include/backend/processing/EModulusLutCatalog.{h,cpp}` (+ frontend `LutHttpFetcher`) | `QNetworkAccessManager`, `QStandardPaths`, `QDir/QFile/QSaveFile`, `QDateTime`, `QUrl`, `QCryptographicHash`, `QEventLoop/QTimer` | yes | injected `HttpGetFn` seam (ADR 0002); nlohmann; `std::filesystem`; `processingCore*Sha256`; ISO strings; injected app-data dir | **done (slice 4)** — `Qt6::Network` dropped |
| 5 | Mock-camera decode | `src/backend/camera/mock/MockCamera.cpp` | `QImage`/`QImageReader` | no | OpenCV `imread` (fallback already present) | **done (slice 2)** — `Qt6::Gui` dropped from the backend link + moved to frontend-only in `MIBDependencies.cmake` |
| 6 | Crash-reporter glue | `src/backend/services/CrashReporter.cpp` (+ dead `QString` include in `AppBackend.cpp`) | `qtMessageHandler`, `QString` | no | Qt log handler moved to frontend `QtLogBridge` (installs `qInstallMessageHandler`, calls back to `captureMessage`) | **done (slice 5)** — `Qt6::Core` dropped, `AUTOMOC OFF`; `mib_backend` fully Qt-free |

`mib_backend` now links **no Qt** (`Qt6::Core` dropped, `AUTOMOC OFF`). The one
thing still forcing `find_package(Qt6 Core)` in the `linux-backend-only` build
is the 7 `frontend;utility` tests that link `Qt6::Core` directly
(`json_flatten_roundtrip`, `json_config_merge`, `processing_core_catalog`,
`application_settings`, `processing_core_settings`, `hdf_review_export_paths`,
`update_catalog`). De-Qt-ing or relocating those, then removing the
`find_package(Qt6)` from `MIBDependencies.cmake` for backend-only, is the
**Phase 1 exit gate** (`linux-backend-only` builds with no Qt SDK).

## Feature-parity matrix

Behavioural parity is the target; deliberate UX changes are tracked separately.

| Workflow | Manual page | Qt | Tauri |
|----------|-------------|----|-------|
| Install + update | getting-started | ships | not started |
| Connect: EGrabber / MindVision / mock | connect | ships | not started |
| Live view, zoom/pan, ROI, overlays, status | acquire-and-record | ships | not started |
| Processing settings, profiles, trust gates, preview | acquire-and-record | ships | not started |
| Experiment lifecycle, monitoring charts, HDF5 recording | acquire-and-record | ships | not started |
| Review, playback, metrics, image/CSV export, reanalysis | review-and-postprocess | ships | not started |
| Autofocus, nanopositioner, syringe pump, trigger | (hardware dialogs) | ships | not started |
| Logs, crash reports, docs, problem reporting | troubleshooting | ships | not started |

## Baseline performance budgets

Fill the "Qt baseline" column from measurements on reference hardware; Tauri
must match or improve each before cutover.

| Metric | Qt baseline | Tauri target |
|--------|-------------|--------------|
| Capture frame rate (fps) | TBD | ≥ baseline |
| Displayed frame rate (fps) | TBD | ≥ baseline |
| Display latency (ms) | TBD | ≤ baseline |
| Dropped frames (%) under load | TBD | ≤ baseline |
| CPU (%) at reference capture | TBD | ≤ baseline |
| Memory (RSS) steady-state | TBD | ≤ baseline |
| Processing throughput | TBD | ≥ baseline |
| HDF5 write throughput | TBD | ≥ baseline |
| Startup / shutdown time (s) | TBD | ≤ baseline |
| Long-duration soak stability | TBD | ≥ baseline |

## Incremental PR strategy

Phase 1 — backend Qt-free (one PR per cluster, each with tests + vault):

1. **[done]** ModbusRtu.h + MindVisionConfig.h contracts → std/nlohmann.
2. **[done]** Mock-camera decode → OpenCV (cluster 5). Dropped `Qt6::Gui` from
   the backend link and moved it to the frontend-only Qt component set.
   (Reordered ahead of serial: smaller, fully Linux-verifiable by the existing
   `camera.mock_smoke` test, and it removed the last backend `Qt6::Gui` user.)
3. **[done]** Serial abstraction: `QSerialPort` behind a platform-neutral
   `ISerialPort` + factory (mirrors the `CameraFactory` DI pattern); ported
   `SyringePumpService` + `scanModbusAddresses` (now fully Qt-free); POSIX
   (termios) + Win32 impls. Dropped `Qt6::SerialPort`. Tests: a `FakeSerialPort`
   Modbus-slave headless round-trip + a real pty-loopback termios test.
4. **[done]** LUT-catalog seam (cluster 4, ADR 0002): catalog is Qt-free
   (nlohmann / `std::filesystem` / `processingCore*Sha256` / ISO strings); the
   HTTP GET is an injected `HttpGetFn` supplied by the shell (Qt frontend
   `LutHttpFetcher` now, Rust later); app-data dir injected for cache-path
   parity. Dropped `Qt6::Network`. Test rewritten Qt-free (file:// state
   machine).
5. **[done]** Crash-reporter glue (cluster 6): the Qt log handler moved to the
   frontend `QtLogBridge` (installs `qInstallMessageHandler`, calls back to
   `CrashReporter::captureMessage`); removed the dead `QString` include in
   `AppBackend.cpp`; **dropped `Qt6::Core` and turned `AUTOMOC OFF`** — so
   `mib_backend` links no Qt. Also de-Qt-ed 6 backend/integration/hardware tests
   that only constructed a throwaway `QCoreApplication` (dead since the LUT
   catalog stopped checking for a Qt app instance). Full suite green (73/73);
   `nm`/`ldd` confirm the backend references zero Qt symbols.
6. **[done — Phase 1 exit gate reached]** Gated the 7 `frontend;utility` tests
   (they link `Qt6::Core` and compile `src/frontend/utils` sources that
   legitimately use Qt — QSettings/QCryptographicHash/QUrl/etc.) behind
   `if(NOT MIB_BUILD_BACKEND_ONLY)` (they still run in the full/Windows build);
   removed `find_package(Qt6)` for backend-only and gated the global
   `CMAKE_AUTOMOC/UIC/RCC` off. `backend-ci.yml` installs no `qt6-*` packages.
   **`linux-backend-only` configures/builds/tests with no Qt SDK** — verified by
   uninstalling Qt6 locally (66/66 green).

Phase 2 — production Rust ↔ C++ bridge (ADR 0003):

7. **[done]** `crates/mib-bridge` — a `cxx` bridge wrapping
   `backend::bridge::BackendFacade`. Rust owns an opaque `BackendBridge`
   (`UniquePtr`) composing `AppBackend` + `BackendFacade`; it exposes lifecycle
   (`initialize`/`shutdown`), flat command submitters (mock-camera configure,
   start/stop capture, start/stop recording, playback-seek), a poll-drained
   event queue (events serialised to a typed-slot `BridgeEvent`, enqueued
   non-blocking on the backend thread), and an on-demand frame pull
   (`fetch_latest_frame` → metadata + one owned byte copy; **no per-frame
   base64**, per principle #4). `build.rs` drives the `linux-backend-only`
   preset to produce the archives and links them; a headless `cargo test`
   contract test runs the full init → configure → start → pull-frame → seek →
   FrameReady → stop → shutdown lifecycle. CI: `bridge-ci.yml` (no Qt, no
   webkit, no display). The command/event set is versioned
   (`bridge_abi_version() == 1`).

Phase 3 — first Tauri vertical slice (mock camera end to end):

8. **[done]** `desktop/` — a React + Tauri v2 app driving the backend through
   `mib-bridge`. `src-tauri` exposes the bridge as `#[tauri::command]`s
   (`init`, `configure_mock`, `start_capture`/`stop_capture`, `seek_latest`,
   `poll_events`, `fetch_frame`, `frame_bytes`); frame pixels ship as a binary
   `tauri::ipc::Response` (**no base64**, principle #4). The React frontend
   (`bridge.ts` + `App.tsx`) configures a mock camera, starts capture, and
   renders live Mono8 frames to a canvas while draining status events.
   Verified headless two ways: a `cargo test` bridge round-trip
   (`mock_camera_slice_round_trip`, asserts 512×96 frames) and an Xvfb GUI
   smoke launch (`desktop/scripts/xvfb-smoke.sh`, WebKitGTK container
   workarounds). CI: `desktop-ci.yml` (frontend build + Tauri build + test +
   Xvfb smoke). Crate-type is `rlib` (binary), since the non-PIC C++ archives
   can't link a `cdylib`. See [[../../knowledge_map/architecture/Desktop-Shell]].
   The webkit deps that were feared to be a hard block install cleanly on
   `ubuntu-24.04`; the GUI runs headless under Xvfb.

Phase 4 — migrate the remaining operator workflows (one slice per PR):

9. **[done — slice 1: recording + review]** Extended `mib-bridge` to schema v2
   with the review commands (`load_recording`, `playback_seek_index`,
   `fetch_frame_by_index`, additive over v1) and exposed them + `start/stop_
   recording` as Tauri commands. The `desktop/` UI gained a Recording panel
   (record the live mock stream to HDF5) and a Review panel (load a recording +
   scrub by frame index, bounded by `PlaybackPosition` events). Headless tests:
   `mib-bridge` `record_then_load_and_review` + desktop
   `record_and_review_round_trip`; Xvfb smoke still green.

10. **[done — slice 2: processing settings + stats overlay]** Added a
    `BackendFacade::fetchProcessingStats` const pull (fps + pixel→micron, over
    `backend_.processing()`), exposed via bridge schema **v3** commands
    `apply_processing(realtime, px→µm)` + `fetch_processing_stats`. The
    `desktop/` app gained a Processing panel (enable realtime + set the
    pixel→micron scale + a live fps overlay polled each tick). Headless tests:
    `mib-bridge` `processing_settings_and_stats` + desktop
    `processing_settings_round_trip`.

Remaining Phase 4 slices (camera selection, experiment run + monitoring, syringe
pump, autofocus/nanopositioner) are specced with per-slice verification notes in
[`2026-07-15-phase4-remaining-slices.md`](2026-07-15-phase4-remaining-slices.md)
— they are hardware-dependent or large/visual, past the point that is fully
verifiable in headless CI. Phase 5 packages, documents, and cuts over.

**PR #59** stays open as reference; it is superseded when the first production
Tauri slice (Phase 3) lands, at which point it is closed with a pointer here.
