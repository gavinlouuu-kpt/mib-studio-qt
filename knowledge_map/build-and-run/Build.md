# Build

> CMake + Conan. Windows (VS2022 x64) is the primary target, with Linux
> cloud builds supported for non-hardware paths.

**Source:** `CMakeLists.txt`, `CMakePresets.json`, `conanfile.py`,
`env/` (`apt-packages.txt`, `brew-packages.txt`, `toolchain.toml`,
`requirements-*.txt`, `assets.json`), `conan/profiles/`,
`scripts/doctor.{sh,ps1}`, `scripts/bootstrap.{sh,ps1}`

## Start here (any host, 2026-09-21)

```bash
scripts/doctor.sh [--sections base,backend,frontend,desktop-shell] [--with-private]
scripts/bootstrap.sh [--sections ...] [--public-assets-only] [--dry-run]
```

The doctor only checks and prints one fix command per missing item; the
bootstrap installs (system packages via apt/brew, a `.venv` with Conan and
NumPy from `env/requirements-build.txt`, the MindVision SDK, the required
assets) and is safe to rerun. Windows: `.\scripts\doctor.ps1` /
`.\scripts\bootstrap.ps1 [-Generator Ninja]` (winget for tools, then
`conan install` with `conan/profiles/windows-msvc194[-ninja]`). Every list
these read has exactly one home:

| Concern | File | Read by |
|---|---|---|
| apt packages, by `# section:` | `env/apt-packages.txt` | doctor/bootstrap; `.devcontainer/Dockerfile`; `.github/actions/setup-linux-env` (every Linux CI lane) |
| Homebrew formulae | `env/brew-packages.txt` | doctor/bootstrap on macOS |
| tool minimums | `env/toolchain.toml` | doctor (flat `name = ">=x"` lines) |
| runtime pins | `rust-toolchain.toml`, `.nvmrc` + `desktop/package.json` engines, `.python-version`, `mise.toml` | rustup, nvm/Node, pyenv/uv, mise |
| Python packages | `env/requirements-build.txt` (conan, numpy), `-scripts.txt`, `-tools-runtime.txt`, `-tools-build.txt` | bootstrap, `tools/build_*`, `scripts/build_*`, CI |
| Conan host profiles | `conan/profiles/linux-gcc13`, `windows-msvc194`, `windows-msvc194-ninja` (carries the `cpuinfo` `[replace_requires]`) | `conan install -pr conan/profiles/<name>`; Windows workflows |
| external datasets / models | `env/assets.json` | `scripts/provision-assets.py`, CMake, harnesses ([[Assets]]) |

## Containers and CI lanes (2026-09-21)

`.devcontainer/` builds `ubuntu:24.04` + sections `base,backend,frontend` of
`env/apt-packages.txt`, a `/opt/venv` with Conan + NumPy, and on create runs
`scripts/bootstrap.sh --skip-packages --public-assets-only` (SDK, `.venv`,
Conan profile, public assets) then the doctor. Named volumes keep the
Hugging Face and Conan caches across rebuilds; `HF_TOKEN` passes through from
the host when set. The image is built per job/container, not published
(decision in the plan). `.github/actions/setup-linux-env` is the one place
Linux workflows get packages (`sections`, `extra-packages`), Conan
(`conan: "true"`), the MindVision SDK and assets; `backend-ci`, `bridge-ci`,
`desktop-ci`, `review-ci` (YOFO Review, [[../frontend/YofoReview]]; builds
`mib_review_core`, the Qt-free review library [[../services/ReviewSession]]
that `mib_backend` and the review bridge link),
`sanitizers`, `soak`, `exporter-soak`, `python-wheel` (Linux)
and `network-tests` all use it, and `grep apt-get .github/workflows` should
match nothing. `network-tests.yml` (nightly + manual) runs
`ctest --preset linux-network-test`; every default test preset excludes the
`network` label.

### CI runtime bounds and compiler cache (2026-10-07)

The Linux lanes use `mozilla-actions/sccache-action@v0.0.11` with the
explicit `v0.16.0` sccache binary and `SCCACHE_GHA_ENABLED=true`. The existing
CMake auto-detection in `cmake/MIBCompilerSettings.cmake` remains the compiler
launcher; the workflows do not duplicate launcher arguments. Cache statistics
are written to the lane log artifact on every attempted run. Hosted cache hit
rates are not proven until a hosted run completes.

Each long-running command has both a GNU `timeout` and a slightly larger
GitHub step budget. `timeout --signal=TERM --kill-after=30s` gives child
processes a grace period before force-killing a stalled process. CTest's
existing per-test timeout and test selection are preserved, and a timeout or
test failure remains non-zero. Logs and CTest failure files upload on success
or failure with a seven-day retention period; path-gated sanitizer jobs do not
upload artifacts for docs-only pull requests.

| Lane / check | Job budget | Command budget / step budget |
|---|---:|---:|
| Sanitizers (each matrix entry) | 60 min | setup 10; configure 4/5; build 35/36; suite 12/13 |
| Backend CI | 45 min | setup 10; Conan probe 180 s advisory; configure 4/5; combined build 20/21; suite 12/13; PL replay 4/5 |
| Nightly soak | 60 min | repeat validation before setup; setup 10; configure 4/5; build 20/21; suite 30/31 |
| Python wheel (x86_64 / ARM64 QEMU) | 20 / 120 min | job-level cap around the unchanged cibuildwheel and test coverage |

The soak input is validated as an integer from 1 through 100 before package
setup; the default remains 40. Its full existing test selection, including
heavy memory-budget and HDF export tests, remains intact. Workflow concurrency
cancels stale runs on the same ref so queued retry churn cannot accumulate.

Observed timing evidence used for these bounds:

- [Backend run 37583515274](https://github.com/gavinlouuu-kpt/mib-studio-qt/actions/runs/37583515274): configure 17 s, build 5 m 48 s, suite 3 m 49 s,
  PL replay 3 s, and the advisory Conan probe 17 s.
- [Sanitizer run 37582104010](https://github.com/gavinlouuu-kpt/mib-studio-qt/actions/runs/37582104010): ASan build about 21 m with a 4 m 13 s test
  portion; TSan build about 5 m 35 s with a 3 m 49 s test portion.
- [Sanitizer run 37562107549](https://github.com/gavinlouuu-kpt/mib-studio-qt/actions/runs/37562107549): 60 m overall wall time, with ASan runtime about
  26 m and TSan runtime about 14 m. Matrix jobs overlap or stagger, so these
  runtimes are not additive queue or billing totals.
- [Soak run 37456065522](https://github.com/gavinlouuu-kpt/mib-studio-qt/actions/runs/37456065522): compile 12 m 48 s and test 12 m 30 s.
- [ARM64 wheel run 37568147020](https://github.com/gavinlouuu-kpt/mib-studio-qt/actions/runs/37568147020): all four Python wheels under QEMU took about
  69.5 m, while x86_64 entries took about 3.0–3.8 m; the larger ARM cap is
  therefore intentional until a native ARM runner is available.

These observations prove a need for bounds and retry/churn control, not a
specific deadlock or a complete billing total. A stale main-checkout view and
queued/cancelled attempts can make hosted totals misleading; the first hosted
run after this change should verify cache behavior and artifact sizes.

## Native FCS export

Build the Qt-free `hdf_export_cli` target for `scripts/export_hdf5.py --format
fcs`. `recording.fcs_writer` and `recording.hdf_export_service` cover the writer
and transaction; `recording.fcs_flowio` runs the optional independent Python
reader and skips when FlowIO is unavailable. See [[../services/HdfExportService]]
and [[../../docs/howto/hdf5-export-app]] for detection semantics and commands.

## Presets

Every configure preset carries a `description` naming the `env/` sections
and Conan profile it expects (`cmake --list-presets` shows them). The shared
file holds no machine-specific paths: put ignores such as a conda or
linuxbrew prefix in `CMakeUserPresets.json` (gitignored; start from
`CMakeUserPresets.example.json`).

From `CMakePresets.json` (each preset's `description` says which `env/`
sections and Conan profile it needs):

| Configure preset | Generator | Toolchain | Use |
|---|---|---|---|
| `windows-default` | VS 2022 x64 | `conan install . -of build -pr conan/profiles/windows-msvc194` | Debug/Release multi-config; installers |
| `windows-ninja` | Ninja, `build-ninja/` | `conan install . -of build-ninja -pr conan/profiles/windows-msvc194-ninja` (VS x64 dev shell) | **fast local loop**; sccache picked up automatically ([[../task/2026-09-09-windows-ninja-fast-loop]]) |
| `windows-ninja-ci` | Ninja, `build/` | same profile, `-of build` | what `build-windows.yml` uses |
| `linux-backend-only` | Makefiles | apt sections `base,backend` | backend libs + CTest, no Qt; CI and devcontainer |
| `linux-system-release` | Ninja | apt sections `base,backend,frontend` | full app from Ubuntu packages |
| `linux-release` / `linux-sentry-release` | Makefiles | `conan/profiles/linux-gcc13` | Conan Qt 6.7.3 on Linux |
| `macos-review-core` | Ninja, `build/review-core/` | `conan install . -of build/review-core -o "&:review_core=True" -pr:h/-pr:b conan/profiles/macos-appleclang-arm64` | YOFO Review: `mib_processing` + `mib_review_core` + link probe, static deps ([[../frontend/YofoReview]], `docs/howto/macos-build.md`) |
| `windows-review-core` | Ninja, `build/review-core/` | same with `conan/profiles/windows-msvc194-ninja` (VS x64 dev shell) | YOFO Review on Windows (`docs/howto/build-installer.md`) |

- Every preset builds with `MIB_USE_SENTRY=ON` (the option's default) so
  CrashReporter's sentry-native paths are compiled and tested everywhere;
  on Linux this needs `libcurl4-openssl-dev` (in section `backend`), and an
  offline fetch degrades gracefully to local-only crash reporting. The wheel
  workflow's plugin builds pass `-DMIB_USE_SENTRY=OFF` explicitly.
- Windows + Ninja requires cl.exe's `/showIncludes` prefix to be detected;
  configure fails otherwise ([[../task/2026-09-15-rig-pc-ninja-showincludes-cpuinfo]]).
- Build presets: `windows-default-build` (Debug),
  `windows-default-build-release`, `windows-ninja-build`, `windows-ninja-ci-build`,
  `linux-*-build`.
- Test presets: `windows-test` / `windows-ninja-test` (fast lane: excludes
  `integration|hardware|soak` — `scripts.exporter_soak` alone is ~10 min and
  belongs to `soak.yml`), `windows-ninja-integration-test`,
  `windows-ninja-hardware-test`, `linux-backend-only-test` (excludes label
  `network`), `linux-network-test` (only `network`).

## Targets

## Optional Aravis Fake validation

Aravis is an optional Qt-free backend dependency and is disabled by default.
For a local Fake-interface validation build, provision the pinned 0.9.3 source
described by `env/aravis.toml` (USB, packet-socket, viewer and GStreamer can be
disabled), then configure with `MIB_ENABLE_ARAVIS=ON` and set
`PKG_CONFIG_PATH` to that prefix. Build `mib_backend_tests` and run the
`camera.aravis_*` plus `backend.aravis_*` CTest cases. `MIB_CAMERA_MODE=aravis`
requires `MIB_ARAVIS_FAKE=1` for the Fake device; it never silently falls back
to the folder replay camera. Adding
`-DMIB_PZ7035_GENTL_CTI=<pz7035-imx426>/gentl/build/libpz7035_gentl.cti`
registers `camera.aravis_pz7035_pattern`, which runs the adapter against the
PZ7035 producer's pattern device (no hardware). See [[../camera/AravisCamera]]
and [[../task/2026-09-27-aravis-framework]].

### ARMv7 cross-build for the PZ7035 PS (YOFO Studio)

The `linux-armv7-yocto` preset cross-compiles the backend, the CTest runner and
`yofo_preview_soak` for the Cortex-A9 with the YOFO Yocto SDK
(pz7035-imx426 `yocto/meta-yofo`: `bitbake yofo-image -c populate_sdk`; the
image carries the matching runtime libraries). Aravis on; Sentry and MindVision
off; OpenCV, spdlog, SQLite, OpenSSL and HDF5 from the SDK sysroot.

```bash
. <sdk>/environment-setup-cortexa9t2hf-neon-amd-linux-gnueabi   # in a clean shell
cmake --preset linux-armv7-yocto && cmake --build --preset linux-armv7-yocto-build -j16
scripts/yofo/deploy_target.sh 20   # strips, copies to the PS, runs scripts/yofo/target_smoke.sh
```

The YOFO Studio server cross-compiles the same way after the CMake tree:
`YOFO_SDK=<sdk> scripts/yofo/cargo-armv7.sh build --release --manifest-path
crates/mib-bridge-server/Cargo.toml` (needs `rustup target add
armv7-unknown-linux-gnueabihf`). The script maps the SDK compilers to cargo's
target-specific variables (the generic `CC`/`CFLAGS` would hit host build
scripts) and points the bridge at `build/linux-armv7-yocto`
(`MIB_BRIDGE_BUILD_DIR`, `MIB_BRIDGE_SYSROOT`); `crates/mib-bridge/build.rs`
links the Aravis libraries recorded in that tree's `CMakeCache.txt`.

`cmake/toolchains/yocto-armv7.cmake` keeps every package search in the sysroot.
HDF5 needs care: the SDK's HDF5 package config is unusable (absolute install
dir, imported targets at `/usr/lib`) and FindHDF5 would otherwise ask the
host's `h5cc` and compile against host headers, so the toolchain skips the
config and gives FindHDF5 a failing wrapper, which makes it search the sysroot.
`target_smoke.sh` runs the backend and Aravis lifecycle tests from the runner,
then `yofo_preview_soak` against the live producer (preview and Overview);
`deploy_target.sh` reads `YOFO_TARGET`, `YOFO_SSH_OPTS` and
`YOFO_SUDO_PASSWORD_FILE`.

#### CI compile smoke (`armv7-smoke.yml`)

The Yocto SDK exists only on the build host, so CI cannot build the shipped
binary. `.github/workflows/armv7-smoke.yml` instead cross-compiles
`mib_processing` and `mib_backend` with `MIB_PL_SCIENCE=ON` using the distro
toolchain (preset `linux-armhf-smoke`, toolchain
`cmake/toolchains/linux-armhf.cmake`, Cortex-A9 NEON hard-float). It catches
32-bit and ARM breaks in the PL code (a `long` assumed 64-bit, `time_t`, an
unguarded Linux-only call); a 64-bit-`long` `static_assert` in a PL source
fails it.

- **Packages:** `scripts/ci/enable-armhf-apt.sh` adds the armhf architecture
  (ports.ubuntu.com; the existing sources become amd64-only). Then
  `setup-linux-env` installs `base,armhf-cross` from `env/apt-packages.txt`.
- **Cost:** the armhf packages take about 8 minutes to install, and the build
  about 2 minutes at 4 jobs (a runner has 4 vCPU; more parallel compiles than
  cores can exhaust memory).
- **Gating:** like `sanitizers.yml`, a path filter gates the steps, not the
  job, so docs-only PRs finish in seconds and the status context exists.
- **After the build:** the job checks the objects are 32-bit ARM hard-float (a
  silently fallen-back host compiler would otherwise pass), and compiles
  `tools/pz_provider_probe/main.cpp` syntax-only.
- **Not covered:** Aravis (Ubuntu's armhf libaravis is older than the 0.9.3 the
  build needs), the Rust server link and the UI, library versions of the real
  image, and any artifact for `meta-yofo`. That job needs the SDK published
  for CI (ADR 0011, decision 14) and is a follow-up.

| Target | Kind | Purpose |
|---|---|---|
| `mib_processing` | STATIC library | Qt-free processing core: `ProcessingService`, `EModulusLut`, `BatchMaskSources`, `Hdf5Service`, `FrameStore`, `Tools`, `CrashStateMirror`. Links only OpenCV + HDF5 + spdlog + STL. |
| `mib_processing_core` | MODULE library | Cross-platform native hot-swap plugin exposing the C engine ABI. Linux builds exercise the `.so` loader; the current signed release output is `build/Release/mib_processing_core-<version>-windows_x86_64.dll` plus the same-stem descriptor JSON. |
| `mib_backend` | STATIC library | Core: services, camera abstraction; links `mib_processing` publicly |
| `mib_frontend_common` | STATIC library | All UI sources (tabs, dialogs, `.ui` files) compiled once and linked by both frontend executables. AUTOUIC runs only here — it is `OFF` on the executables because CMake would emit duplicate `ui_*.h` generation rules per target; the `.qrc` is compiled per-executable so the Qt resources survive static linking |
| `mib_studio_qt` | executable (`WIN32` on Windows) | Production app (mock camera reachable via ConnectTab "Configure Mock…" or `MIB_CAMERA_MODE=mock`) |
| `screenshot_tour` | executable | Headless UI tour that regenerates the user-manual screenshots (`docs/manual/images`); builds on Linux too (`linux-system-release`); see [[../frontend/Screenshot-Tour]] |
| `processing_core_dialog_test` | executable test | Offscreen Qt regression proving the local active-core identity remains visible when registry loading fails; generated only by full frontend builds (`ctest -R frontend.processing_core_dialog`) |
| `mib_backend_tests`, `mib_frontend_tests` | executable test runners | **One binary per test group** (2026-09-09, `cmake/MIBTestRunner.cmake`): each registered test source is compiled with `main=mib_test_main__<target>` and dispatched on the first argument, so CTest runs `<runner> <target> [args]` (one process per test). Run one directly: `build/Release/mib_backend_tests.exe emodulus_lut_catalog_test`; `--list` prints the names. `mib_backend_tests` holds the 72 backend tests (eleven stay standalone: `MIB_STANDALONE_BACKEND_TESTS` in `tests/CMakeLists.txt`, each with its reason); `mib_frontend_tests` holds the eight widget tests and compiles `resources/defaults.qrc` itself. The per-test executables summed 345 s of link time on the bench PC; the two runners link in a few seconds. |
| `mib_backend_smoke_test` | executable test | Backend-only HDF5/open/flush smoke test (`ctest -L backend`); standalone because `tools/gen_bridge_link_manifest.py` reads its `.vcxproj` |

`mib_backend` is linked by every executable. Source is in
`src/backend/`, `src/camera/`, and `src/backend/playback/`.

`mib_processing` is defined first in `src/backend/CMakeLists.txt` and has
`AUTOMOC`/`AUTOUIC`/`AUTORCC` explicitly off — it must not require the Qt
`moc` toolchain to build standalone. `backend-ci.yml` builds it explicitly
and greps its symbols to fail CI if a Qt dependency leaks back in. This is
the artifact a non-Qt consumer (Biowork's `services/mib-processing`) is
meant to build/bind against — see `docs/gold_standard_metrics.md` ("Portable
Processing Contract") and the [Biowork portability
epic](https://github.com/KPT1020/mib-studio-qt/issues/220).

Backend-only builds set `MIB_BUILD_BACKEND_ONLY=ON` and skip frontend target
generation entirely, but still require Qt `Core+Gui+SerialPort+Network`
because the backend now fetches the LUT manifest directly at startup.

## Python bindings (`bindings/python/`)

`_mib_processing` is a pybind11 extension module (`bindings/python/src/`)
linking `mib_processing`; the importable package is `mib_processing`
(`bindings/python/python/mib_processing/`, thin re-export layer). Built via
[scikit-build-core](https://scikit-build-core.readthedocs.io), driven by
`bindings/python/pyproject.toml`, which points `cmake.source-dir` at the
repo root so it configures the *same* root `CMakeLists.txt` (option
`MIB_BUILD_PYTHON_BINDINGS=ON`, set automatically by the wheel build) rather
than a separate CMake project:

```bash
cd bindings/python
pip install .              # or: pip install -e . --no-build-isolation (dev loop)
python -m pytest tests/
cd ../..
python scripts/run_processing_conformance.py  # installed-wheel anti-drift check
```

Requires the same system packages as `linux-backend-only`/
`linux-system-release` (`docs/howto/linux-build.md`). Wheel builds set
`MIB_BUILD_PROCESSING_ONLY=ON`, which stops after the Qt-free library and
bindings; Qt, cameras, and desktop services are not configured. `mib_processing`
has `POSITION_INDEPENDENT_CODE ON` (needed to link a static library into a
shared `.so` extension module); this has no effect on the desktop static/
executable link.

CI produces repaired `manylinux_2_28` wheels for CPython 3.10–3.13 on
`x86_64` and `aarch64`. cibuildwheel installs/imports every
wheel in its matching container (QEMU-backed for ARM64 on the GitHub x86_64
runner); the x86_64 jobs retain the full pytest/conformance suite and import
CPython 3.12 in a slim production base with Biowork's `libgl1` and
`libglib2.0-0` prerequisites. AlmaLinux/EPEL supplies the build dependencies
before `auditwheel` repair. Linux i686 is intentionally unsupported because
NumPy no longer publishes those wheels and the workload is not a practical fit
for a 32-bit address space. See `bindings/python/README.md`.
ARMv7 is excluded because cibuildwheel 2.22 marks it experimental and lacks a
CPython 3.12 manylinux ARMv7 target.

`.github/workflows/python-wheel.yml` builds + tests on every relevant PR then
runs the full-parity conformance harness before publishing wheels as GitHub
Release assets on `mib-processing-v*` tags
(a separate tag namespace from the app's own `v*.*.*` releases,
`.github/workflows/release.yml`).

The same workflow is the processing-core release gate. Its wheel matrix covers
all 8 CPython/architecture pairs, a Windows x64 job builds the native core
artifact, and a tag release rejects any incomplete wheel set before
`publish-processing-core.py --from-release`
updates the R2 registry. Publication order is immutable
`processing-core/versions/<version>.json`, merged `index.json`, generated PEP
503 page, then the backward-compatible full `latest.json` pointer. A tag build
fails when production Authenticode or R2 secrets are absent; local/PR builds
may exercise the unsigned fixture path but cannot publish it.

The Windows audit logs raw `dumpbin` exports/dependents, accepts its optional
alias suffix when parsing export rows, and still requires exactly one export
named `mib_processing_get_api` with no Qt/HDF5/OpenCV/Python/app DLL imports.

Use `python scripts/bump_mib_processing_version.py <version>` to update both
the authoritative pyproject and package wrapper literal. After committing,
rerun with `--create-tag`; the script verifies that `HEAD` contains the version
before creating `mib-processing-v<version>`.

## Native processing-core plugin

`mib_processing_core` compiles only the bundled mask/empty-frame kernel and C
ABI adapter; it does not link Qt, HDF5, or the desktop service graph. Its
version comes from the same `bindings/python/pyproject.toml` literal as the
wheel. On Windows, CMake emits a same-stem descriptor containing the exact
version, ABI/contract, `mib_processing_get_api` entrypoint, runtime
fingerprint, compatible app range, and required Authenticode scheme.

Release CI runs the ABI, loader, cache, and activation tests, signs the DLL,
attaches the signed DLL/descriptor beside the wheels, then lets the registry
publisher calculate the final byte size and SHA-256. The desktop rechecks the
registry digest and compiled Authenticode signer-SPKI allowlist immediately
before `LoadLibraryExW`; unsigned debug fixtures are never publishable.

The C ABI contract is in
`include/backend/processing/ProcessingCoreAbi.h`. Keep it POD-only and
append-compatible: no STL/OpenCV/Qt objects, exceptions, RTTI, or ownership
transfer may cross this boundary. `tests/processing/processing_core_abi_c_test.c`
is deliberately compiled as required C11 on every compiler, while loader parity
and cache/concurrency tests exercise the dynamic artifact.

The backend and sanitizer CI lanes install NumPy for the external-HDF5
conformance-input test.
The public Hugging Face Dataset Viewer integration retries remote requests and
uses CTest exit 77 only for exhausted HTTP 429/5xx, connection, or timeout
failures; malformed dataset payloads and scientific regressions still fail.

Production desktop builds must configure
`-DMIB_REQUIRE_PROCESSING_CORE_SIGNER_SPKI=ON` and
`-DMIB_PROCESSING_CORE_SIGNER_SPKI_SHA256=<64-hex>` with the approved signer's
DER SubjectPublicKeyInfo SHA-256. CMake rejects missing, non-hex, or
non-64-character text and normalizes valid pins to lowercase. Official stable/beta workflows
source the value from the repository Actions variable; the native release job
also compares it with the signer certificate extracted from the signed DLL.
The requirement defaults off so local and fork CI builds remain possible, but
an unpinned Release build cannot load a native core and is not distributable;
environment overrides are limited to Debug builds.

The Windows native job builds pure C/C++ good, truncated, incompatible,
malformed, and exception-containing modules from a separately configured
fixture project. It audits the production DLL for exactly one exported ABI
entrypoint and rejects Qt/HDF5/OpenCV/Python/app imports. OpenCV is linked
statically for that artifact. Tag releases require explicit app bounds through
`MIB_PROCESSING_CORE_APP_{MIN,MAX}_VERSION`; development defaults both to the
current desktop version.

## Desktop release safety

The desktop has three maintained publishers: local `release.ps1`, manual
`.github/workflows/build-windows.yml`, and tag-triggered
`.github/workflows/release.yml`. The `develop` auto-beta trigger in
`build-windows.yml` is path-gated: merges touching only docs, the vault,
`tools/`, `tests/`, `bindings/python/`, markdown, or other workflows do not
build or publish a beta (changes to that workflow itself still release).
Processing-core `mib-processing-v*` GitHub releases pass `--latest=false` so
the repository "Latest" badge always names the newest stable desktop release.
Each publisher cleans `build/dist`, derives the numeric
installer artifact version separately from a possible `-beta.*` release tag,
and requires the exact Setup and Update filenames before hashing or publishing.
Wildcards are limited to cleanup/unexpected-output detection; GitHub Release,
Actions artifact, and R2 inputs are exact paths.

`scripts/resolve_desktop_release_version.py` resolves the greater of the
fallback literal and all reachable stable/beta tag numeric versions before any
bump. Publishers pass paired one-configure
`MIB_RELEASE_VERSION_{,FULL_}OVERRIDE` values; `MIBVersion.cmake` validates that
they share one numeric identity, removes them from the cache, and writes
`build/mib-release-identity.txt` for a pre-build readback gate. This prevents a
stale fallback or prior beta tag from changing the tested binary identity.
Inno Setup and GitHub retain numeric filenames; `publish-update.py` maps beta
bytes to an immutable full-version R2 object key and orders equal numeric SHA
betas by publication time.

Manual stable CI prepares the version in its workspace, then runs build, CTest,
installer, validation, and artifact-upload gates before creating the commit and
tag. It verifies `origin/main` is still the tested dispatch SHA and atomically
pushes both refs. The tag workflow validates its requested tag, checks out that
exact ref, compares the resolved tag commit with `HEAD` before configure, and
runs CTest before Sentry publication. The local publisher also builds the full
default target set, runs CTest before installers, atomically pushes branch/tag,
and treats GitHub/R2 failures as fatal. It requires a clean named branch, and a
pushed stable release is restricted to `main` (beta may use a feature branch).
The local publisher reports a prospective dry-run version, treats installer
failures/missing exact outputs as fatal, and refuses to move an existing tag.

The top-level CMake project enables both C and CXX so Ubuntu system-package
HDF5 discovery can run its C probe while the application code remains C++17.
`cmake/MIBLinkHelpers.cmake` accepts both the namespaced HDF5 targets used by
Conan/Linux packages and the un-namespaced `hdf5-shared` / `hdf5-static`
targets exported by upstream/Homebrew HDF5 2.x. With OpenCV 5 it additionally
links `opencv_geometry`; OpenCV 4 keeps the existing imgproc-only path.

## Commands

```bash
# Configure (once; after scripts/bootstrap.ps1, which runs conan install with conan/profiles/windows-msvc194)
cmake --preset windows-default

# Build Debug
cmake --build build --config Debug

# Build Release
cmake --build build --preset windows-default-build-release

# Fast local loop (VS 2022 x64 developer shell; see the windows-ninja preset above)
conan install . -of build-ninja --build=missing -s build_type=Release -pr conan/profiles/windows-msvc194-ninja
cmake --preset windows-ninja
cmake --build --preset windows-ninja-build
ctest --preset windows-ninja-test -j8

# Backend-only (Linux)
cmake --preset linux-backend-only
cmake --build --preset linux-backend-only-build --target mib_backend mib_backend_smoke_test emodulus_lut_catalog_test
ctest --preset linux-backend-only-test -L backend --output-on-failure

# Deploy Qt runtime (CMake auto-triggers post-build; can run manually)
windeployqt.exe --release build/Release/mib_studio_qt.exe
```

## Conan

Dependencies resolved via Conan 2 (`conanfile.py`; host profiles in
`conan/profiles/`). See [[Dependencies]]. Option `review_core=True` (YOFO
Review) resolves only spdlog, HDF5, OpenCV (four modules, no FFmpeg /
protobuf / Eigen) and nlohmann_json, with OpenCV and HDF5 **static**; the
default graph is unchanged (shared OpenCV / HDF5, Qt, SQLite, ONNX Runtime on
Windows) — the shared/static choice moved from `default_options` to
`configure()`. Post-build hooks call
`windeployqt.exe` to copy Qt plugins and DLLs next to the exe. CMake resolves
`windeployqt` and the `PATH` prefix from Conan CMakeDeps’ `qt_PACKAGE_FOLDER_*`
so Release/Debug tools stay aligned with the linked Qt package (stale
`find_program` cache or cache-wide `Qt6Core.dll` globs could otherwise mix
different Conan package IDs after reinstalls).

## Platform guards (hardware SDKs)

- `CMakeLists.txt` sets `MIB_HAS_EGRABBER`:
  - `ON` on Windows
  - `OFF` on non-Windows
- `cmake/MIBOptions.cmake` adds `MIB_ENABLE_MINDVISION`:
  - `ON` by default on Windows, Linux, and macOS
  - callers may explicitly use `OFF` only for a deliberate SDK-free stub build
- `cmake/MIBDependencies.cmake` sets `MIB_HAS_MINDVISION`:
  - `ON` when `MIB_ENABLE_MINDVISION=ON` for desktop/backend builds
  - `OFF` for processing-only builds, which do not compile camera services
- `MIB_HAS_COREMOR` independently detects the Windows CoreMOR SDK. Windows
  defaults `MIB_ENABLE_COREMOR=ON` and builds the bundled XMT driver even when
  `MIB_ENABLE_HARDWARE_SDKS=OFF` disables EGrabber; set `MIB_ENABLE_COREMOR=OFF`
  for a build without it. Linux and processing-only builds are SDK-free for
  Coremor (vendor inventory: [[../services/AutofocusService]]).
- When `MIB_HAS_EGRABBER=OFF`, build wiring skips:
  - EGrabber include path (`C:/Program Files/Euresys/eGrabber/include`)
- When `MIB_HAS_COREMOR=OFF`, build wiring skips the CoreMOR import library;
  the full autofocus service and OEABT serial backend still build.
- `MIB_BUILD_OEABT_TOOLS=ON` builds `oeabtctl` in the build root.
- `MIB_BUILD_STAGE_TOOLS=ON` (default) builds `zc300ctl` in the build root.
  It is the ZC300 Z-stage diagnostic: `info` and `status` are read-only,
  `move` needs `--allow-motion`, and `configure` needs `--allow-write`. It
  links `stage_zc300` → `stage_zc300_protocol` + `oeabt_serial` (which also
  carries `SerialBus.cpp`), not the backend. See [[../services/ZC300Stage]].
  `mib_backend` itself links `stage_zc300` (for [[../services/StageService]]),
  so the Rust bridge's archive list in `crates/mib-bridge/build.rs` names
  `stage_zc300` and `stage_zc300_protocol`.
- When `MIB_HAS_MINDVISION=ON`, CMake requires:
  - Windows: `CameraApiLoad.h` plus `MVCAMSDK.dll` / `MVCAMSDK_X64.dll`
  - Linux/macOS: `CameraApi.h` plus `libMVSDK.so` / `libmvsdk.dylib`
  - SDK root overrides via `MIB_MINDVISION_SDK_ROOT` or the
    `MIB_MINDVISION_SDK_DIR` environment variable; a separate runtime path may
    be supplied with `MIB_MINDVISION_RUNTIME_DIR`
- Official beta/stable workflows and `release.ps1` run
  `scripts/provision-mindvision-sdk.ps1`, which checksum-verifies the pinned
  R2 vendor installer, extracts the build headers/runtime without installing
  drivers on CI, and fails the release if `MVCAMSDK_X64.dll` is absent from
  the deployed payload.
- Linux backend, sanitizer, soak, and native-core CI run
  `scripts/provision-mindvision-sdk.sh`; the same command provisions local
  Linux/macOS build trees from the platform/architecture-specific R2 archive.
- When MindVision is disabled, the backend still compiles a stub camera
  implementation and the connect UI keeps mock/EGrabber workflows intact.

## Linux cloud toolchain note (`cannot find -lstdc++`)

Some cloud images pin `/usr/bin/c++` to clang without an unversioned
`libstdc++.so`; switch the alternative to g++. Details and the smoke test:
[[../task/2026-04-20-cloud-toolchain-cxx-libstdcpp-fix]].

## Related how-tos

- `docs/howto/build-installer.md`
- `docs/howto/windows-deploy.md`
- `docs/howto/runtime-deploy.md`
- `docs/howto/release-workflow.md`

## Profile registry foundation (#398)

Registry sources are part of `mib_backend`; `profiles.registry` is in backend CTest.
They use existing nlohmann JSON, SQLite and shared SHA-256 without Qt. Optional
PostgreSQL policy tests run with `npm ci --prefix supabase && npm test --prefix supabase`
(pinned PGlite development dependency); the `profile-registry-ci.yml` lane runs
them on `supabase/**` changes. No new desktop run mode is enabled.

## Windows Authenticode test target

`processing_core_authenticode_test` stays a standalone executable because the
Python-wheel workflow and `scripts/test-processing-core-authenticode.ps1` invoke
it directly with unsigned/signed fixture paths and a signer SPKI hash. Bundling
it into `mib_backend_tests` removes the expected MSBuild target and breaks that
release verification. The standalone-test list in `tests/CMakeLists.txt` preserves
this contract.

## History

Dated build notes live in `knowledge_map/task/` (this note states current
truth only):

- [[../task/2026-09-15-rig-pc-ninja-showincludes-cpuinfo]] — localized cl.exe prefix, `cpuinfo` conflict
- [[../task/2026-09-09-windows-ninja-fast-loop]] — Ninja preset measurements, sccache, Rust bridge on the Ninja tree
- [[../task/2026-09-15-hardware-shutdown]] — shutdown regression targets and why two desktop tests are standalone
- [[../task/2026-09-15-mindvision-overview-roi]] — MindVision overview test coverage
- [[../task/2026-04-20-cloud-toolchain-cxx-libstdcpp-fix]] — `-lstdc++` cloud image fix
- [[../task/2026-06-01-backend-only-build-test-mode]] — origin of `MIB_BUILD_BACKEND_ONLY`

### Native GTK workflow acceptance

The production-webview harness in `desktop/scripts/native-workflow.py` accepts
the GTK folder picker with its real Open button while retaining the typed
location. Escape discards that location and is not a valid acceptance action.
The export gate asserts the exact selected destination parent, not just terminal
success. Idle exit verifies native-window disappearance if WebKit closes its
session before replying. Failure artifacts include the full X11 desktop so
native dialogs are visible alongside webview screenshots.

Windows Tauri candidate staging resolves model files with the same manifest root
and `<kind>s/<id>/<file>` layout as the provisioner/CMake, honors `MIB_ASSETS_DIR`,
and checks the declared SHA256 before packaging. The GTK harness tolerates only
confirmed dialog unmapping between window search and focus; other X11 failures
remain errors.

Native packaged acceptance also regenerates a new HDF through the real UI/save
dialog, verifies the source file digest is unchanged, waits for the frontend
terminal status, and reopens the regenerated output with processing-core identity.

Fresh GTK Recent mode is explicitly left via Home before folder selection; the
harness uses in-memory GSettings to avoid depending on an operator desktop.
Idle Exit verifies both native-window disappearance and owning-process exit via
X11/procfs, avoiding hung WebDriver requests after its last window closes. It
also avoids deleting the terminated session; the driver process is cleaned up.

The Xvfb smoke script bounds and terminates its entire owned process group via
GNU timeout, not only the xvfb-run wrapper. A real-child regression reproduces
and prevents orphan applications holding the executable open during bundling.
The smoke run uses disposable XDG state and mock-camera mode, never an operator
profile or remembered hardware configuration.
The alive timer starts inside Xvfb, after display startup; the regression delays
display startup deliberately to prevent a false pass before the app launches.

The Windows candidate saves successfully provisioned Conan dependencies before
application compilation, so subsequent source/test failures do not discard the
completed dependency cache. It never caches a failed dependency install.

Release documentation (#573): `scripts/release_notes.py --version X.Y.Z --check`
gates stable Windows releases before the build. GitHub releases use the curated
Markdown file; beta builds generate uncurated notes from recent fragments after
the last reachable stable tag. `publish-update.py --release-notes-file` includes a
16 KiB plain-text summary in latest.json and catalog entries. Qt deployment and
both installers copy release notes and the manual with images under resources;
Linux builds also copy these beside the executable (there were no app install rules). Tauri bundles the same
sources via Vite raw/URL imports. Templates live in `docs/release-notes/README.md`.
