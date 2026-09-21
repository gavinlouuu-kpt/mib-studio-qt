# Testing Strategy

This document describes how to validate changes while preserving the backend
and frontend boundaries documented in this architecture set. For the
developer-facing recipe book (how to actually write each kind of test, with
templates and a PR checklist) see [`../howto/writing-tests.md`](../howto/writing-tests.md).

## Network lane (2026-09-21)

Tests that reach the Hugging Face Hub carry the CTest label `network` and
`SKIP_RETURN_CODE 77` (transient 429/5xx or connection failure counts as a
skip, schema/regression failures still fail). Every default test preset
excludes the label; `linux-network-test` includes only it and
`.github/workflows/network-tests.yml` runs it nightly and on demand with
`HF_TOKEN` from repository secrets for private corpora. Dataset identity comes
from `env/assets.json` (see `knowledge_map/build-and-run/Assets.md`).

## Test Layers

| Layer | Current entry point | Use when |
| --- | --- | --- |
| Documentation checks | Markdown/content review plus `git diff --check` | Docs-only changes, architecture updates, runbooks, and review evidence generation. |
| Backend configure/build | `cmake --preset linux-backend-only` and `cmake --build --preset linux-backend-only-build` | Backend services, camera abstractions, processing, storage, and CTest targets on Linux. |
| Backend CTest | `ctest --preset linux-backend-only-test` | Service behavior, processing pipelines, HDF5/playback behavior, and backend regressions. |
| Windows app build | `cmake --preset windows-default` and `cmake --build --preset windows-default-build` or release preset | Qt Widgets app, packaging-sensitive code, Windows hardware SDK paths, and installer/runtime changes. |
| Windows CTest | `ctest --preset windows-debug-test` or `ctest --preset windows-debug-integration-test` | Windows-specific backend validation and integration tests. |
| Script/tool checks | Script-specific commands under `scripts/` or `tools/` | Standalone post-processing tools, Python helpers, and generated evidence workflows. |
| Manual/runtime evidence | Screenshots, recordings, sample images, metrics, and logs in a review bundle | UI, CV/image-processing, pipeline, hardware, or workflow changes where command output alone is insufficient. |

## Test Categories (by failure mode)

The layers above describe *where* tests run. Tests are also classified by *what
failure mode they guard*, because the bugs that break this app (deadlocks, lost
wakeups, stale frames, save-path failures, latency backlog) are not "levels."
Each category has a canonical example already in `tests/`.

| Category | Guards against | Example |
| --- | --- | --- |
| Unit / behavior | logic regressions | deterministic service tests |
| Round-trip | silent data loss/corruption | `recording_lifecycle_test` |
| Fault-injection | save fails on some drives/paths/states | `e2e_storage_destinations_test` |
| Pipeline e2e | broken experiments / silent frame loss | `kin6_mib_app_capture_proof` |
| Concurrency / lifecycle stress | deadlocks, races, crashes on start/stop | `e2e_pipeline_stress_test` |
| Invariant / property | torn / stale / evicted-aliased frames | `frame_store_concurrency_test`, `frame_store_bounds_test` |
| Performance / latency budget | growing lag, variable/dropped triggers | `e2e_live_view_latency_test`, `e2e_trigger_timing_test` |
| Soak (nightly) | slow leaks, accumulation, rare races | (nightly job) |

## Capability Coverage Matrix

A change to a core capability must land with its required categories. This is the
gate, mirrored in [`AGENTS.md`](../../AGENTS.md).

| Capability / area | Required categories |
| --- | --- |
| Save data (`Hdf5Service`, export, recording, config) | Round-trip **+** Fault-injection |
| Run experiments (capture/processing/recording lifecycle) | Pipeline e2e **+** Concurrency stress |
| Real-time (processing, trigger, display, `FrameStore`) | Latency budget **+** Invariant |
| Any code touching threads / shared state | Passes the TSan lane and has/extends a stress test |

If a required category does not exist yet for the touched area, creating it is
part of the change.

## Cross-cutting Rules

- **Regression-first:** a bug fix lands with a test proven to fail before the fix.
- **No naked `join()`/`wait()` that can hang CI:** thread/pipeline tests install a
  watchdog that prints the stuck location and `_Exit(99)`s. Use `_Exit`, not
  `abort()` (the linked crash handler intercepts `abort()` and can itself hang).
- **Timing tests gate on steady-state or ratios, not absolute milliseconds**, so
  they are machine-independent. Mark probabilistic tests; give them generous,
  stable thresholds.
- **Frame accounting is conserved:** pipeline tests assert captured == processed
  + explicitly dropped (no silent loss).

## CI Lanes

- **`backend-ci`** (Linux, PRs): configure/build/test the backend-only preset.
- **Sanitizer lane** (Linux, PRs): backend-only built with `-fsanitize=thread`,
  plus an `-fsanitize=address,undefined` variant, running unit / invariant /
  round-trip / stress tests. ThreadSanitizer catches the lost-wakeup / data-race
  class directly.
- **Nightly soak** (cron): stress / soak / latency tests at high cycle/Hz
  counts; non-gating, uploads logs.
- Windows lanes (`ci.yml`, `build-windows.yml`, `release.yml`) pinned to
  `windows-2022` to match the VS2022 / msvc-194 toolchain.

## Shared Test-Support Library

`tests/support/` (built incrementally) removes copy-paste across tests:
`assert.h`, `watchdog.h` (RAII watchdog with `mark()` + `_Exit`), `tempdir.h`,
`mock_pipeline.h` (AppBackend + mock camera fixture), `hdf5_roundtrip.h`
(round-trip + fault-injection helpers), `frames.h` (synthetic + real-dir frame
sources), `stats.h` (latency/percentile collectors).

Design rationale and the prioritized rollout backlog:
[`../superpowers/specs/2026-06-23-testing-framework-design.md`](../superpowers/specs/2026-06-23-testing-framework-design.md).

## Current Registered C++ Tests

The root CMake file registers backend tests when `BUILD_TESTING` is enabled.
Current test names include:

- `backend.smoke`
- `backend.processing_batch_pipeline`
- `backend.kin10_hf_dataset_pipeline`
- `backend.kin6_mib_app_capture_proof`
- `backend.processing_multi_object`
- `backend.processing_object_tracking`

These tests link `mib_backend`. That is intentional: backend behavior should be
validatable without launching the Qt Widgets application.

## Validation By Change Type

### Docs-Only Changes

For architecture and runbook-only changes:

- confirm the documented paths and target names exist in the repository
- run `git diff --check`
- run a structural check that required markdown files are present and non-empty
- generate review evidence with `report.html`, `manifest.json`, command logs,
  and a flow diagram when the ticket asks for one
- confirm `git diff --name-only` contains only expected documentation or review
  artifact files

Docs-only changes do not require CMake configure/build unless they alter build,
source, test, or runtime files.

### Backend Changes

For backend code, camera code, processing, storage, diagnostics, or public
backend headers:

1. Configure the backend-only build:

   ```bash
   cmake --preset linux-backend-only
   ```

2. Build the touched backend target or the backend-only preset:

   ```bash
   cmake --build --preset linux-backend-only-build
   ```

3. Run targeted CTest first, then the broader backend suite:

   ```bash
   ctest --preset linux-backend-only-test -R <test-name> --output-on-failure
   ctest --preset linux-backend-only-test --output-on-failure
   ```

4. Capture configure, build, and test logs with exit codes in the review bundle.

### Frontend Changes

For Qt Widgets, UI resources, tabs, dialogs, frontend controllers, models, or
app startup:

- build the app target through the relevant Windows or Linux preset available
  in the environment
- run backend tests when the frontend path calls or changes backend behavior
- launch the app when possible and capture screenshots or a short demo for the
  changed path
- record launch failures as evidence instead of silently skipping runtime
  validation

### Pipeline, CV, Or Image-Processing Changes

For processing, segmentation, HDF5 pipeline, and visual output changes:

- run targeted processing tests
- use representative samples, not a single happy path
- save input, mask/output, overlay/contour images, and per-sample metrics
- include aggregate metrics and the exact regeneration command in
  `manifest.json`

### Hardware Or External Integration Changes

For EGrabber, Coremor, syringe pump, updater, Sentry, RustFS, or network-backed
integrations:

- keep hardware-independent logic covered by backend tests where possible
- document unavailable hardware or credentials in the review bundle
- run mock-mode or stub-mode validation on Linux when hardware is unavailable
- run Windows/hardware validation when the change cannot be proven through
  mocks or stubs

## Adding Tests With New Code

Use these placement rules:

- Add backend service tests under `tests/` (mirroring the source area, e.g.
  `tests/backend/`, `tests/processing/`, `tests/integration/`) and register them
  with CTest in `tests/CMakeLists.txt`.
- Link backend tests to `mib_backend`, not `mib_studio_qt`.
- Keep integration tests labeled `integration` so fast presets can exclude
  network or dataset-backed work.
- Keep small committed fixtures under `tests/fixtures/` when needed.
- Keep generated test outputs under `build/test-output/` or
  `review_artifacts/`; both are local artifact locations and should not become
  runtime inputs.
- Add Python tests under `tests/` when validating Python scripts or fixtures.

## Review Evidence Expectations

Every review bundle should include enough evidence for a reviewer to audit the
change without reading the agent transcript:

- exact commands, timestamps, exit codes, working directory, and logs
- source commit and branch metadata
- `report.html` for human review
- `manifest.json` for machine-readable artifact provenance
- diagrams for architecture, data-flow, pipeline, UI-flow, or orchestration
  changes
- samples and metrics for image-processing or UI-visible work

For this documentation-only architecture phase, the required evidence is a docs
consistency review, `git diff --check`, a structural markdown check, an
architecture flow diagram, `report.html`, and `manifest.json`.
