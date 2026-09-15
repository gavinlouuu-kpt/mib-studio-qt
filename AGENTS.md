# Agent Guide — MIB Studio Qt

C++17 / Qt 6.7.3 microscopy app: OpenCV imaging, Euresys EGrabber and
MindVision cameras, HDF5 storage, and optional ONNX YOLO.

This file is a map, not a manual. Deep knowledge lives in the vault and docs.

## Onboarding

1. [`knowledge_map/Agent-Onboarding.md`](knowledge_map/Agent-Onboarding.md) — guided ~10 min ramp-up
2. [`knowledge_map/README.md`](knowledge_map/README.md) — vault map of content
3. [`docs/golden-principles.md`](docs/golden-principles.md) — mechanical rules for this repo

## Key Navigation

| Task | Start here |
|------|------------|
| Architecture, threading, data flow | [`knowledge_map/architecture/Overview.md`](knowledge_map/architecture/Overview.md) |
| A backend service | `knowledge_map/services/<Name>Service.md` |
| A frontend tab/dialog/controller | `knowledge_map/frontend/` |
| Build, run modes, dependencies | [`knowledge_map/build-and-run/Build.md`](knowledge_map/build-and-run/Build.md) |
| Code and logging conventions | [`knowledge_map/conventions/Code-Conventions.md`](knowledge_map/conventions/Code-Conventions.md) |
| How-to guides and runbooks | [`docs/README.md`](docs/README.md) |
| Architecture decisions (ADRs) | [`docs/decisions/README.md`](docs/decisions/README.md) |
| Multi-PR or design-heavy work | [`docs/exec-plans/README.md`](docs/exec-plans/README.md) |
| Known debt | [`docs/exec-plans/tech-debt-tracker.md`](docs/exec-plans/tech-debt-tracker.md) |
| What shipped recently | [`knowledge_map/current-state/Recent-Work.md`](knowledge_map/current-state/Recent-Work.md) |

## Vault Maintenance (required)

Every code change must land with matching vault updates in the same
commit/PR. The source-file to vault-note mapping is in
[`knowledge_map/Vault-Maintenance.md`](knowledge_map/Vault-Maintenance.md).

## Build and Run

```bash
./scripts/provision-mindvision-sdk.sh      # Linux/macOS; populates build/vendor
cmake --preset linux-backend-only         # Linux, backend lib + tests only
cmake --build --preset linux-backend-only-build
```

Windows: run `./scripts/provision-mindvision-sdk.ps1`, then configure/build the
`windows-default` preset. MindVision is default-on for desktop builds;
`MIB_BUILD_PROCESSING_ONLY=ON` remains SDK-free.

- `mib_studio_qt` — the app; mock camera via ConnectTab "Configure Mock…" or
  `MIB_CAMERA_MODE=mock` + `MIB_MOCK_CAMERA_DIR=<path>` (see
  [`knowledge_map/build-and-run/Run-Modes.md`](knowledge_map/build-and-run/Run-Modes.md))
- `screenshot_tour` — headless UI tour regenerating the user-manual
  screenshots ([`docs/manual/README.md`](docs/manual/README.md))

**Fresh cloud agent / container:** provision packages first (`apt-get update` —
the index is stale). Ubuntu Noble's Qt 6.4.2 builds and passes backend CTest; or
build only `mib_processing` for the fastest Qt-free loop. Exact commands:
[`docs/howto/linux-build.md`](docs/howto/linux-build.md).

## Verification

```bash
python3 scripts/check_docs.py                  # docs + vault wikilink integrity
python3 scripts/check_screenshots.py           # user manual <-> screenshot harness sync
ctest --preset linux-backend-only-test         # backend unit tests
```

CI: [`backend-ci.yml`](.github/workflows/backend-ci.yml) builds and tests the
backend on Linux; [`docs-ci.yml`](.github/workflows/docs-ci.yml) runs the
knowledge checks; [`ci.yml`](.github/workflows/ci.yml) validates Windows
packaging scripts; [`docs-site.yml`](.github/workflows/docs-site.yml)
publishes `docs/manual/` as the user-guide website (`mkdocs.yml`).

## Testing framework (safeguards)

Tests are bare `main()` executables linked to `mib_backend` and registered with
CTest (no test framework). They are organized by failure mode, not just by
level. How to write tests (recipes, support library, templates, checklist):
[`docs/howto/writing-tests.md`](docs/howto/writing-tests.md). Full taxonomy, CI
lanes, and rationale:
[`docs/architecture/testing-strategy.md`](docs/architecture/testing-strategy.md).

**Coverage matrix — a change to a capability lands with its required tests:**

| Capability / area | Required test categories |
|------|------|
| Save data (`Hdf5Service`, export, recording, config) | Round-trip **+** Fault-injection |
| Run experiments (capture/processing/recording **lifecycle**) | Pipeline e2e **+** Concurrency stress |
| Real-time (processing, trigger, display, `FrameStore`) | Latency budget **+** Invariant |
| Any code touching threads / shared state | Passes the **TSan lane** and has/extends a stress test |

If a required category does not exist yet for the touched area, creating it is
part of the change.

**Mandatory rules:**

- **Regression-first:** every bug fix lands with a test proven to fail before
  the fix and pass after.
- **No naked `join()`/`wait()` that can hang CI:** thread/pipeline tests install
  a watchdog that prints the stuck location and `_Exit(99)`s. Use `_Exit`, not
  `abort()` (the crash handler intercepts `abort()`).
- **Timing tests gate on steady-state/ratios, not absolute milliseconds** so
  they are machine-independent; mark probabilistic tests.
- **Pipeline tests assert frame accounting is conserved** (captured ==
  processed + explicitly dropped — no silent loss).

## Hard Conventions

- **Know your architecture layer before changing backend code**
  ([`Overview`](knowledge_map/architecture/Overview.md), [`backend-boundaries`](docs/architecture/backend-boundaries.md)).
  The Qt-free processing core in `src/backend/processing/` is an ABI-stable, signed, swappable plugin —
  preserve `ProcessingCoreAbi.h` + gold-standard conformance; a behavior change needs a version bump + re-sign.
- **spdlog** for logging; never `std::cout` in app code.
- Headers mirror source layout under `include/`.
- Review existing `Tools` ([`src/backend/app/Tools.cpp`](src/backend/app/Tools.cpp)) before writing new utilities.
- Prefer ready-made EGrabber SDK patterns over hand-rolled camera code.
- Runtime data (logs, sqlite, HDF5, mock frames) lives under `data/`.
- Test performance metrics go to MLflow at `mlflow.yofo.bio` via
  `MLFLOW_TRACKING_USERNAME` / `MLFLOW_TRACKING_PASSWORD` env vars (never hardcode).
