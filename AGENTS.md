# Agent Guide — MIB Studio Qt

C++17 / Qt 6.7.3 microscopy app: OpenCV imaging, Euresys EGrabber and
MindVision cameras, HDF5 storage, optional ONNX YOLO. This file is a map, not
a manual: start with [`knowledge_map/Agent-Onboarding.md`](knowledge_map/Agent-Onboarding.md),
then [`docs/golden-principles.md`](docs/golden-principles.md) (mechanical rules).

## Architecture (top → bottom; dependencies point down only)

| Layer | Where | Notes |
|---|---|---|
| Desktop shells | `src/frontend/` (Qt Widgets: MainWindow, tabs, controllers) · `desktop/` (React + Tauri) | Reach the backend only through `AppBackend` / `BackendFacade`, never service internals |
| Rust bridge | `crates/mib-bridge/` (cxx) + `contract/bridge-contract.json` | Frontend-neutral facade for the Tauri shell; additive, versioned ABI (ADR 0003/0004) |
| Composition root | `src/backend/app/AppBackend.cpp` | Owns every service and the shared `FrameStore` ring buffer; `shutdown()` orders teardown |
| Services | `src/backend/services/` (Capture, Processing, Hdf5, Recorder, Playback, Trigger, Autofocus, CameraControl, SyringePump, Yolo, Sqlite) · `src/backend/discovery/` | One concern per service; vault note per service under `knowledge_map/services/` |
| Portable core | `mib_processing` static lib: `src/backend/processing/`, `FrameStore`, `Hdf5Service`, `Tools` | **Qt-free** (OpenCV + HDF5 + spdlog); ABI-stable signed plugin `mib_processing_core`; pybind11 wheel in `bindings/python/` |
| Cameras | `src/camera/` behind `ICamera` | EGrabber (Windows), MindVision (all desktops), Mock (folder playback) |

Capture, processing, trigger, autofocus and discovery each own a thread; UI
work stays on the main thread; handoff is ring buffer, callbacks, atomics.
Details: [`Overview`](knowledge_map/architecture/Overview.md), [`Threading-Model`](knowledge_map/architecture/Threading-Model.md),
[`backend-boundaries`](docs/architecture/backend-boundaries.md), [`project-structure`](docs/architecture/project-structure.md).

## Key Navigation

| Task | Start here |
|------|------------|
| A backend service / a frontend tab | `knowledge_map/services/<Name>Service.md` / `knowledge_map/frontend/` |
| Build, run modes, dependencies, assets | [`knowledge_map/build-and-run/Build.md`](knowledge_map/build-and-run/Build.md) |
| Code and logging conventions | [`knowledge_map/conventions/Code-Conventions.md`](knowledge_map/conventions/Code-Conventions.md) |
| How-tos, ADRs, execution plans, known debt | [`docs/README.md`](docs/README.md), [`docs/decisions/README.md`](docs/decisions/README.md), [`docs/exec-plans/README.md`](docs/exec-plans/README.md), [`tech-debt-tracker`](docs/exec-plans/tech-debt-tracker.md) |
| What shipped recently | [`knowledge_map/current-state/Recent-Work.md`](knowledge_map/current-state/Recent-Work.md) |

**Vault maintenance is required:** every code change lands with matching vault
updates in the same PR ([`knowledge_map/Vault-Maintenance.md`](knowledge_map/Vault-Maintenance.md)).

## Build and Run

```bash
scripts/doctor.sh          # what this host is missing, with the exact fix commands
scripts/bootstrap.sh       # install it: packages, Conan, MindVision SDK, assets (env/)
cmake --preset linux-backend-only && cmake --build --preset linux-backend-only-build
```

Windows: `.\scripts\doctor.ps1`, `.\scripts\bootstrap.ps1`, then the
`windows-default` (VS) or `windows-ninja` (fast loop) preset. Every setup list
lives once under `env/` + `conan/profiles/` (see Build.md). Fresh container:
`scripts/bootstrap.sh --public-assets-only` (`apt-get update` first; details in
[`docs/howto/linux-build.md`](docs/howto/linux-build.md)). Targets: `mib_studio_qt`
(mock camera via ConnectTab "Configure Mock…" or `MIB_CAMERA_MODE=mock` +
`MIB_MOCK_CAMERA_DIR`, see [`Run-Modes`](knowledge_map/build-and-run/Run-Modes.md));
`screenshot_tour` (regenerates the user-manual screenshots, [`docs/manual/README.md`](docs/manual/README.md)).

## Test data and models (Hugging Face)

Every dataset and model weight is pinned in [`env/assets.json`](env/assets.json)
(Hub id, commit, SHA-256, consumers) and fetched into `build/vendor/assets/` by
the stdlib provisioner. Never hardcode a Hub id in code (`check_docs.py` fails).
Public assets need no token; bumping a pin is a reviewed edit to the manifest.
Guide: [`knowledge_map/build-and-run/Assets.md`](knowledge_map/build-and-run/Assets.md).

```bash
python3 scripts/provision-assets.py --list                                   # what exists, public/private
python3 scripts/provision-assets.py --required-only                          # yolo11n-seg.onnx (ONNX builds)
python3 scripts/provision-assets.py --asset 512x96stream-mock-frames --count 1000   # 512x96 TIFF stream -> MIB_MOCK_CAMERA_DIR
HF_TOKEN=<read token> python3 scripts/provision-assets.py --asset z-adjustment-50v  # private 1.5 GB HDF5 conformance corpus
python3 scripts/provision-assets.py --check --required-only                  # offline: exit 1 + fix command if incomplete
ctest --preset linux-network-test   # `network`-labelled tests (kin10) pull rows live via the Dataset Viewer API
```

## Verification

```bash
python3 scripts/check_docs.py                  # docs + vault wikilink integrity, Hub ids declared
python3 scripts/check_screenshots.py           # user manual <-> screenshot harness sync
ctest --preset linux-backend-only-test         # backend unit tests (excludes label `network`)
```

CI: Linux lanes install via [`setup-linux-env`](.github/actions/setup-linux-env/action.yml)
(reads `env/apt-packages.txt`); `backend-ci.yml` builds and tests the backend,
`sanitizers.yml` runs TSan/ASan+UBSan, `docs-ci.yml` the knowledge checks,
`ci.yml` Windows script syntax, `docs-site.yml` publishes `docs/manual/`.

## Testing framework (safeguards)

Tests are bare `main()` executables linked to `mib_backend`, registered with
CTest and organized by failure mode. Recipes, support library and the
mandatory rules (regression-first, watchdog + `_Exit(99)` instead of naked
joins, ratio-based timing gates, conserved frame accounting):
[`docs/howto/writing-tests.md`](docs/howto/writing-tests.md); taxonomy and CI
lanes: [`docs/architecture/testing-strategy.md`](docs/architecture/testing-strategy.md).

**Coverage matrix — a change to a capability lands with its required tests
(create the category if it does not exist yet):**

| Capability / area | Required test categories |
|------|------|
| Save data (`Hdf5Service`, export, recording, config) | Round-trip **+** Fault-injection |
| Run experiments (capture/processing/recording **lifecycle**) | Pipeline e2e **+** Concurrency stress |
| Real-time (processing, trigger, display, `FrameStore`) | Latency budget **+** Invariant |
| Any code touching threads / shared state | Passes the **TSan lane** and has/extends a stress test |

## Hard Conventions

- **Know your layer (table above) before changing backend code.** The portable
  core is an ABI-stable, signed, swappable plugin: preserve `ProcessingCoreAbi.h`
  + gold-standard conformance; a behavior change needs a version bump + re-sign.
- **spdlog** for logging; never `std::cout` in app code.
- Headers mirror source layout under `include/`.
- Review existing `Tools` ([`src/backend/app/Tools.cpp`](src/backend/app/Tools.cpp)) before writing new utilities.
- Prefer ready-made EGrabber SDK patterns over hand-rolled camera code.
- Runtime data (logs, sqlite, HDF5, mock frames) lives under `data/`.
- Test performance metrics go to MLflow at `mlflow.yofo.bio` via
  `MLFLOW_TRACKING_USERNAME` / `MLFLOW_TRACKING_PASSWORD` env vars (never hardcode).
