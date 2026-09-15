# Linux Build

Use this path for fast local compile verification when changing portable app
code. It disables Windows-only EGrabber/Coremor and installer packaging, but
MindVision is enabled by default from the pinned Linux SDK on R2.

## Prerequisites

- CMake 3.21+
- A C++17 compiler toolchain
- Ninja

For the fastest local loop on Ubuntu, install system development packages:

```bash
sudo apt install cmake build-essential ninja-build pkg-config git \
  qt6-base-dev qt6-charts-dev qt6-serialport-dev \
  libopencv-dev libhdf5-dev libspdlog-dev nlohmann-json3-dev libsqlite3-dev
```

For backend-only build/test loops (no frontend binaries), this smaller set is
enough:

```bash
sudo apt install cmake build-essential pkg-config git \
  qt6-base-dev qt6-serialport-dev \
  libopencv-dev libhdf5-dev libspdlog-dev nlohmann-json3-dev libsqlite3-dev
```

Provision the architecture-matched MindVision headers/shared library before
configuring either desktop preset:

```bash
./scripts/provision-mindvision-sdk.sh
```

CMake discovers this default `build/vendor` output automatically. If you pass
`--destination`, export `MIB_MINDVISION_SDK_ROOT=<destination>/extracted` and
`MIB_MINDVISION_RUNTIME_DIR=$MIB_MINDVISION_SDK_ROOT/lib` before configuring.

The archive includes `libMVSDK.so` for x86, x86_64, arm, and arm64. For USB
cameras, install the extracted udev rules on the target workstation and reload
udev; the provisioner deliberately does not use `sudo` or alter host policy.

## Configure And Build

Fast local build from system packages:

```bash
cmake --preset linux-system-release
cmake --build --preset linux-system-release-build
```

Conan-based build:

```bash
conan install . --profile:host=conan/profiles/linux-gcc13 --profile:build=default --output-folder=build/linux --build=missing
cmake --preset linux-release
cmake --build --preset linux-release-build
```

The `linux-release` preset sets:

- `MIB_ENABLE_HARDWARE_SDKS=OFF`
- `MIB_ENABLE_MINDVISION=ON`
- `MIB_ENABLE_WINDOWS_PACKAGING=OFF`
- `MIB_USE_SENTRY=ON`

That builds EGrabber as a stub, MindVision as the real SDK-backed provider,
and compiles sentry-native in (all presets default Sentry ON; the fetch
degrades gracefully to local-only crash reporting when offline). On Linux
the sentry build needs `libcurl4-openssl-dev`; pass `-DMIB_USE_SENTRY=OFF`
to skip it entirely.

## Cloud agent / fresh container setup

A freshly cloned cloud container ships `cmake`/`ninja`/`g++` but **no Qt, no
OpenCV/HDF5/spdlog, and an empty Conan cache**, so none of the presets above
configure until you provision system packages. Two paths, fastest first.

First, always refresh apt — the base image index is stale and 404s otherwise:

```bash
sudo apt-get update
```

**Fastest loop — Qt-free processing core.** The `mib_processing` static lib
(portable processing contract) needs only OpenCV + HDF5 + spdlog, no Qt:

```bash
sudo apt-get install -y --no-install-recommends \
    libopencv-dev libhdf5-dev libspdlog-dev libfmt-dev nlohmann-json3-dev

cmake -S . -B build/proc-only -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DMIB_BUILD_PROCESSING_ONLY=ON -DMIB_BUILD_BACKEND_ONLY=ON \
    -DBUILD_TESTING=OFF -DMIB_USE_SENTRY=OFF   # BUILD_TESTING must be OFF here: tests/ links Qt
cmake --build build/proc-only --target mib_processing
```

Covers `ProcessingService`, `ProcessingScience`, `FrameStore`, `Hdf5Service`,
`PipelineTimingRecorder`, `CrashStateMirror`, `Tools`, `EModulusLut`, the
kernels. To exercise a pure core function, link a throwaway `main()` against
`build/proc-only/libmib_processing.a` with `pkg-config --cflags --libs
opencv4`, `-lspdlog -lfmt`, and the HDF5 serial libs (`-lhdf5_serial
-lhdf5_serial_cpp`). Does **not** cover `TriggerService`, `CaptureService`,
`AutofocusService`, `src/frontend/`, or the CTest suite (all link Qt).

**Full backend + tests — install Qt from apt.** Qt6 provisions cleanly from
Ubuntu packages (Noble ships Qt **6.4.2**, older than the pinned 6.7.3 but it
configures, builds `mib_backend` + `mib_frontend_common`, and passes the whole
`linux-backend-only` CTest suite):

```bash
sudo apt-get install -y --no-install-recommends \
    qt6-base-dev qt6-charts-dev qt6-serialport-dev libsqlite3-dev \
    libopencv-dev libhdf5-dev libspdlog-dev libfmt-dev nlohmann-json3-dev \
    libcurl4-openssl-dev   # sentry-native curl transport (presets default Sentry ON)

./scripts/provision-mindvision-sdk.sh

cmake --preset linux-backend-only          # or linux-system-release for the frontend libs
cmake --build --preset linux-backend-only-build
ctest --preset linux-backend-only-test     # `pip install numpy` for the conformance script test
```

Prefer this whenever you touch Qt-dependent code so it is locally verified
rather than left to `backend-ci.yml`.

## Compile Sentry Integration

All presets now build with `MIB_USE_SENTRY=ON` by default, so touching
`CrashReporter` code gets sentry-native compile (and test) coverage in the
regular `linux-backend-only` / `linux-release` loops — no dedicated preset
needed. The separate `linux-sentry-release` preset remains for the
Conan-toolchain variant with its own binary dir:

```bash
conan install . --profile:host=conan/profiles/linux-gcc13 --profile:build=default --output-folder=build/linux-sentry --build=missing
cmake --preset linux-sentry-release
cmake --build --preset linux-sentry-release-build
```

If sentry-native or system curl dependencies are unavailable (offline or
minimal containers), configure with `-DMIB_USE_SENTRY=OFF` — or just let the
fetch fail: the build degrades to local-only crash reporting with a CMake
warning.
