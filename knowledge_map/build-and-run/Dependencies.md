# Dependencies

> Third-party stack. Managed by Conan (`conanfile.txt`).

| Package | Version | Shared? | Notes |
|---|---|---|---|
| qt | 6.7.3 | ✓ | **Frontend-only** (epic #246 Phase 1 complete). The backend is fully Qt-free and `MIB_BUILD_BACKEND_ONLY` does **not** `find_package(Qt6)` at all — `linux-backend-only` configures/builds/tests with **no Qt SDK installed** (proven by `backend-ci.yml`, which installs no `qt6-*` packages). The full/frontend build uses Core + Gui + Network + Widgets + Charts + Concurrent + ImageFormats. |
| spdlog | 1.17.0 | | Logging ([[../conventions/Logging]]) |
| sqlite3 | 3.51.0 | | [[../services/SqliteService]] |
| hdf5 | 1.14.6 | ✓ | C++ API enabled — [[../services/Hdf5Service]] |
| opencv | 4.12.0 | ✓ | dnn=False, openexr=False. Linked modules: `core`, `imgproc`, `imgcodecs`, `videoio` (AVI read/write by [[../data-model/FrameStore]] and [[../services/BatchMaskSources]]) — [[../services/ProcessingService]] |
| onnxruntime | 1.18.1 | | Optional in Linux cloud builds; when unavailable the build uses `YoloService.stub.cpp` and disables YOLO runtime features while keeping the rest of the app buildable — [[../services/YoloService]] |
| nlohmann_json | 3.11.3 | | Config parsing / serialization |
| openssl (libcrypto) | system | ✓ | Linux desktop builds only (`find_package(OpenSSL REQUIRED)` under `UNIX AND NOT APPLE`): Ed25519 detached-signature verification for native processing cores. Optional for the Qt-less wheel configure, whose verifier then fails closed. |

## Vendored / checked-in

- **Euresys EGrabber SDK** — `egrabber-sample-programs/` (reference source
  tree); actual SDK is assumed installed system-side. See
  `docs/integration/egrabber.md`.
- **MindVision SDK** — not vendored in git. Every desktop OS enables it by default.
  Local builds discover an installed SDK through `MIB_MINDVISION_SDK_ROOT` /
  `MIB_MINDVISION_SDK_DIR` (and optional `MIB_MINDVISION_RUNTIME_DIR`).
  Windows paths use `scripts/provision-mindvision-sdk.ps1`; Linux/macOS use
  `scripts/provision-mindvision-sdk.sh`. The scripts fetch SHA-256-pinned team
  R2 artifacts and extract only the headers plus the current platform/CPU's
  shared library.
- **Coremor XMT DLL** — `include/Coremor/` (`.h`, `.lib`, `.dll`). Used by
  [[../services/AutofocusService]].

## How they're wired

- `CMakeLists.txt` calls `find_package(Qt6 ...)` and so on; Conan
  generates the toolchain (`build/conan_toolchain.cmake`).
- `mib_backend` links **no Qt** and has `AUTOMOC OFF` (epic #246): `Gui`,
  `Network`, `SerialPort` were all removed (OpenCV mock-camera decode;
  [[../services/ISerialPort]]; injected LUT HTTP seam, ADR 0002), and `Core`
  went with the crash-reporter handler moving to the frontend.
- **`MIB_BUILD_BACKEND_ONLY=ON` requires no Qt at all** (Phase 1 exit gate):
  `MIBDependencies.cmake` skips `find_package(Qt6)` entirely, the global
  `CMAKE_AUTOMOC/UIC/RCC` are gated off, and the 7 `frontend;utility` tests
  (which link `Qt6::Core` and compile `src/frontend/utils` sources) are gated
  behind `if(NOT MIB_BUILD_BACKEND_ONLY)` in `tests/CMakeLists.txt` — they still
  build/run in the full/Windows build. Verified by uninstalling the Qt SDK and
  configuring/building/testing `linux-backend-only` clean.
- Qt shared DLLs + plugins are deployed next to the exe via
  `windeployqt.exe` in a post-build step.
- Full frontend test builds use Qt Widgets in offscreen mode for the
  `processing_core_dialog_test`; backend-only builds do not generate that
  target.
- OpenCV and HDF5 DLLs are also copied next to the exe (see
  `docs/howto/windows-deploy.md`).
- Windows-only hardware SDK linkage is gated by `MIB_HAS_EGRABBER`
  (`WIN32` => `ON`, otherwise `OFF`):
  - EGrabber headers/system path are only added when `MIB_HAS_EGRABBER=1`.
  - Coremor include/lib wiring is only added when `MIB_HAS_EGRABBER=1`.
- MindVision SDK linkage is gated separately by `MIB_ENABLE_MINDVISION` /
  `MIB_HAS_MINDVISION`:
  - CMake locates the dynamic-loader header/DLL on Windows and the direct API
    header/shared library on Linux or macOS.
  - Desktop and backend-only presets set `MIB_ENABLE_MINDVISION=ON`; Linux CI
    provisions the pinned SDK before configure.
  - Processing-only builds remain SDK-free so portable processing artifacts
    do not acquire camera dependencies.
- `QtNetwork` is linked by the backend library so `AppBackend` can manage the
  Young's modulus LUT manifest/cache directly during startup.

## Qt decoupling in progress (epic #246)

`mib_backend` is now **fully Qt-free** — the migration to React + Tauri (ADR
`docs/decisions/0001-react-tauri-migration.md`) removed Qt from the backend
cluster by cluster: `ModbusRtu.h` frames are `std::vector<uint8_t>` (was
`QByteArray`); `MindVisionConfig.h` parses with `nlohmann_json` (was Qt JSON);
the mock camera decodes via OpenCV `cv::imread` (was `QImage` → `Qt6::Gui`
dropped); the syringe pump uses [[../services/ISerialPort]] (was `QSerialPort` →
`Qt6::SerialPort` dropped); the E-modulus LUT catalog delegates HTTP to a
shell-injected seam (ADR 0002 → `Qt6::Network` dropped); and the crash-reporter
Qt log handler moved to the frontend `[[../frontend/System-Utilities]]`
(`QtLogBridge`) so the last `QString`/`qtMessageHandler` usage left the backend
→ `Qt6::Core` dropped and `AUTOMOC` turned off. Finally the 7 `frontend;utility`
tests (which link `Qt6::Core`) were gated behind `if(NOT MIB_BUILD_BACKEND_ONLY)`
and `find_package(Qt6)` was removed for backend-only — so **`MIB_BUILD_BACKEND_ONLY`
now builds and tests with no Qt SDK**, the Phase 1 exit gate. Tracked in
`docs/exec-plans/active/2026-07-15-qt-decoupling-and-tauri-migration.md`.

**Phase 2 — Rust bridge:** `crates/mib-bridge` is a `cxx` crate that links the
Qt-free static archives (`libmib_backend.a` / `libmib_processing.a`) and drives
them through `backend::bridge::BackendFacade` — see [[../architecture/Rust-Bridge]]
and ADR `docs/decisions/0003-rust-cxx-bridge.md`. Toolchain: Rust stable +
`cxx`/`cxx-build`; no Qt, no webkit. Its `build.rs` drives the
`linux-backend-only` preset to produce the archives (skip with
`MIB_BRIDGE_NO_CMAKE=1` if already built) and links them plus OpenCV / HDF5 /
SQLite / spdlog / fmt / crypto. Runtime needs
`LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu/hdf5/serial` on Ubuntu. CI:
`.github/workflows/bridge-ci.yml` (headless `cargo test`).

**Phase 3 — React + Tauri desktop app:** `desktop/` is the Tauri v2 shell — see
[[../architecture/Desktop-Shell]]. Toolchain: Node 22 + npm (React + Vite + TS
frontend), Rust stable + Tauri v2, and the Linux WebKitGTK stack
(`libwebkit2gtk-4.1-dev`, `libgtk-3-dev`, `libsoup-3.0-dev`,
`libjavascriptcoregtk-4.1-dev`) — all install cleanly on `ubuntu-24.04`. The GUI
is smoke-tested headless under `xvfb` (`desktop/scripts/xvfb-smoke.sh`) with the
container WebKitGTK workarounds (`WEBKIT_DISABLE_DMABUF_RENDERER=1`,
`WEBKIT_DISABLE_COMPOSITING_MODE=1`, `LIBGL_ALWAYS_SOFTWARE=1`). CI:
`.github/workflows/desktop-ci.yml`.

## Linux cloud build notes

- If CMake fails before project checks with
  `/usr/bin/ld: cannot find -lstdc++`, verify which compiler `/usr/bin/c++`
  resolves to. Some images pin `c++` to a clang alternative while missing the
  matching unversioned `libstdc++.so` path for that toolchain.
- In those environments, switching `c++` to `g++` resolves the runtime linker
  path issue:
  - `sudo update-alternatives --set c++ /usr/bin/g++`

## Related

- [[Build]] for presets and commands.
- `docs/howto/runtime-deploy.md` for runtime deployment details.
