# Dependencies

> Third-party stack. Managed by Conan (`conanfile.txt`).

| Package | Version | Shared? | Notes |
|---|---|---|---|
| qt | 6.7.3 | ✓ | Core + Gui + SerialPort + Network + Widgets + Charts + ImageFormats |
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
- **MindVision SDK** — not vendored. Configure with
  `MIB_ENABLE_MINDVISION=ON` on Windows and point CMake at the SDK root via
  `MIB_MINDVISION_SDK_ROOT` or `MIB_MINDVISION_SDK_DIR`. The build expects
  the MindVision include tree plus `MVCAMSDK.dll` or `MVCAMSDK_X64.dll`.
- **Coremor XMT DLL** — `include/Coremor/` (`.h`, `.lib`, `.dll`). Used by
  the optional Windows backend of [[../services/AutofocusService]]. OEABT uses
  Qt SerialPort and the operating system's standard serial driver instead of a
  vendored library.

## How they're wired

- `CMakeLists.txt` calls `find_package(Qt6 ...)` and so on; Conan
  generates the toolchain (`build/conan_toolchain.cmake`).
- Backend-only builds (`MIB_BUILD_BACKEND_ONLY=ON`) require Qt
  `Core+Gui+SerialPort+Network` components. Frontend-only modules
  `Widgets`, `Charts`, and `Concurrent` are still not required.
- Qt shared DLLs + plugins are deployed next to the exe via
  `windeployqt.exe` in a post-build step.
- Full frontend test builds use Qt Widgets in offscreen mode for the
  `processing_core_dialog_test`; backend-only builds do not generate that
  target.
- OpenCV and HDF5 DLLs are also copied next to the exe (see
  `docs/howto/windows-deploy.md`).
- Windows EGrabber linkage is gated by `MIB_HAS_EGRABBER`:
  - EGrabber headers/system path are only added when `MIB_HAS_EGRABBER=1`.
- CoreMOR is gated independently by `MIB_HAS_COREMOR`; OEABT targets build on
  Linux and Windows from standard C++17 plus Qt SerialPort.
- MindVision SDK linkage is gated separately by `MIB_ENABLE_MINDVISION` /
  `MIB_HAS_MINDVISION`:
  - CMake locates the MindVision SDK include tree and runtime DLL only when
    `MIB_ENABLE_MINDVISION=ON` on Windows.
- Default builds keep MindVision disabled so Linux/cloud CI does not require
  proprietary SDK files.
- `QtNetwork` is linked by the backend library so `AppBackend` can manage the
  Young's modulus LUT manifest/cache directly during startup.

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
