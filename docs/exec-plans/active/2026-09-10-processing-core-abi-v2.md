# Processing-core ABI v2 execution

Status: active — phases 0–1 validated on Linux; Windows qualification deferred

Source of truth: [#301](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/301).
Execution plan: [#394](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/394).
Inventory reference: develop `e51c72a` (2026-09-10).

## Goal

Switch between legacy subtract/nested-ring and absdiff/Laplacian science on
Windows and Linux, between operations, with exact profile and run provenance.
Deliver the issue's small PR sequence; keep Contract 1 frozen while adding v2.

## Ownership inventory

| Concern | Current entry points / owner | Migration constraint |
|---|---|---|
| Mask | `ProcessingService::processMaskWithActiveKernel` -> `IProcessingKernel::processMask`; bundled implementation in `BundledProcessingKernel.cpp`, native adapter in `ProcessingCoreLoader.cpp` | ABI 1 transports only mask and empty decisions |
| Empty classification | `classifyFrameWithActiveKernel` -> `isImageEmptyWithActiveKernel`; realtime empty/auto-background branches also call the helper | Historical directional mode differs from the explicit absolute flag used by realtime comparison |
| Offline/batch frame | `computeProcessedFrame`, `processBatch`, async batch workers in `ProcessingService.cpp` | Keep leases, queues, record accumulation and tracking lifecycle host-owned |
| Realtime branches | `realtimeLoop` calls mask, empty and `filterProcessedObjects` in inline, async/multi-image and ROI processing paths | Every branch must eventually consume one coherent v2 transaction |
| Contours and metrics | `filterProcessedImage` -> `filterProcessedObjects` -> selected kernel `analyzeObjects`; default invokes `ProcessingScience.cpp` | ABI 1 plugins inherit host science; plugin mask equivalence is not full scientific rollback |
| Ring science | `findContours`, `evaluateInnerContourObject`, `calculateRingRatio`, `classifyInvalidReasons` in `ProcessingScience.cpp` | Freeze tree retrieval, noise cutoff, hull area and sqrt(outer area - inner area), including strict ring bounds |
| Target decisions | Inner/outer object evaluators set `isTargetGroup`; realtime emits first target in deterministic order | Science belongs to core; callback scheduling and trigger device belong to host |
| Tracking | `matchTrackWithActiveKernel` -> kernel `matchTrack` -> `science::findMatchingTrack` | Matching decision is science; BatchTrack lifetime/deduplication is host-owned |
| HDF5 | `Hdf5Service.cpp` compound `ProcessedFrameMetadataRecord`, append/read paths, core identity attributes | Historical `ringRatio` and contract/ABI metadata must remain readable |
| Export/reanalysis | `HdfExportService.cpp`, `ReviewExport.cpp`, Python binding converters and `scripts/export_hdf5.py`, `reanalyse_hdf5.py`, `run_processing_conformance.py` | Add schema dispatch and optional metric presence; no historical relabeling |
| Profiles | `ProfileManager::listProfiles`, `ProcessingCoreCatalog::isProcessingContractCompatible`, `ProcessingConfigJson.cpp`, `AppConfigWatcher.cpp` | Current app bounds + exact contract check; do not silently rewrite existing configs |
| Selection | `ProcessingCoreDialog`, `ProcessingCoreSettings`, `ProcessingService::activateProcessingKernel` | Preserve trust, hard pin, pre-commit persistence callback and operation leases |

All source names above resolve under `src/backend/processing` unless an owner
or directory is specified. Frontend selection/profile owners are under
`src/frontend/dialogs`, `src/frontend/utils`, and `src/frontend/system`.

## Contract/ABI assumptions and identity

`ProcessingCoreAbi.h` fixes engine ABI and processing contract to 1. Bundled
release identity is 0.2.1, sourced from `bindings/python/pyproject.toml` by
`src/backend/CMakeLists.txt`; build ID is `mib-processing-0.2.1` and source is
`bundled`. Runtime fingerprint includes compiler/platform information.

`ProcessingCoreLoader.cpp` rejects requirements outside host ABI/contract,
checks exact version/fingerprint and descriptor/table, verifies hash and trust,
then calls self-test. Windows uses restricted `LoadLibraryExW`; Linux uses
`dlopen(RTLD_NOW | RTLD_LOCAL)`. Modules remain resident. None of these checks
may be relaxed merely to make an item selectable. `ProcessingCoreCatalog.cpp`
parses manifest schema 2: that schema number is not processing Contract 2.

## Ring-specific integration surfaces

- Config: `ProcessingTypes.h` ring limits/check, single-inner-contour policy;
  `ProcessingConfigJson.cpp`, profile managed paths and Python `config_convert.h`.
- Qt: `ProcessingSettingsDialog`, `ProcessingConfigDraft`, `ConfigTabs`,
  `FrameViewerDialog`, `ExperimentMonitoringTab`, `HdfReviewTab`, `HdfMetricsModel`.
- Feedback: `ProcessingService` ring callback, `AutofocusService`,
  `NanopositionerTab`, `AppBackend` wiring.
- React/bridge: `BackendFacade`, `desktop/src/bridge.ts`, `desktop/src/App.tsx`,
  `desktop/src-tauri/src/lib.rs`.
- Persistence/export: HDF5 compound member `ringRatio`, review/CSV exporters,
  Python `filter_result_convert.h`, gold-standard JSON dataset/schema/comparator.
- Diagnostics: `CrashStateMirror` carries ring metrics.

Contract 2 needs capability-aware controls and absent metrics throughout these
surfaces. A ring value of zero cannot stand in for unavailable Laplacian data.

## Phase 0 fixture coverage

The existing `processing.science_golden` test remains the frozen numeric oracle;
new cases extend it without changing production code or any old expected value.

| Fixture | Frozen evidence |
|---|---|
| Empty, bright, darker-than-background | Complete mask bytes + empty classification + borrowed image immutability |
| Difference exactly at threshold | Strict threshold boundary produces zero mask |
| No background | Direct intensity threshold path |
| ROI edge/clipping | Complete clipped mask, zero exterior; existing science border validity case |
| Morphology-sensitive speck | Non-empty pre-morph candidate, zero final mask |
| Multiple ring objects | Existing ordered geometry, brightness, area, deformability, ring metrics |
| Ring valid/invalid | Explicit 30–35 ring window; invalid object cannot trigger, metric retained |
| Tracking sequence | Existing three-frame drift/stationary track goldens |
| Target/non-target | Existing calibrated target window plus ring validity interaction |

Inputs are deterministic source-defined fixtures; pixel goldens use explicit
expected rectangles/zeros, not calls to the algorithm under test. The existing
numeric oracle tolerance remains absolute 1e-9. Do not regenerate expected
values from a candidate core. New pixel fixtures pass against the unchanged C++ reference on Linux.

## Acceptance and remaining delivery

- [x] Inventory current seam, assumptions, identity and ring surfaces.
- [x] Expand Contract-1 regression cases without changing production science.
- [x] Execute expanded goldens against unchanged reference on Linux.
- [ ] Execute expanded goldens on Windows; native CI target repair awaiting rerun.
- [ ] Complete reusable machine-readable fixture/result harness for bundled/native cores.
- [x] Phase 1: coherent internal difference policy and proven failing/passing regression.
- [ ] Phase 2: structured compatibility reasons and UI tests.
- [ ] Phase 3: freeze Contract-2 config/result/Laplacian semantics with #297–299.
- [ ] Phases 4–6: v2 POD ABI, negotiation, buffers and bundled host adapter.
- [ ] Phases 7–8: self-contained legacy and absdiff/Laplacian implementations.
- [ ] Phases 9–11: atomic activation, capabilities, profiles, UI and HDF5 provenance.
- [ ] Phases 12–15: signed Windows/Linux artifacts, conformance and lifecycle qualification.

## Decision log

- 2026-09-10: Reuse the existing science golden test; do not create a competing
  oracle or change its pinned numeric values. Add a watchdog for service teardown.
- 2026-09-10: Empty classification is pre-morphology. Equal difference policies
  do not imply that every non-empty candidate yields a nonzero final mask.
- 2026-09-10: Local environment has no CMake/OpenCV development packages.
  `apt-get update` fails on setgroups/seteuid permissions. Leave C++ validation
  explicitly pending and do not progress to science/ABI changes behind this gate.

## Local build unblocked (2026-09-10)

Downloaded CMake 4.4.3, Ninja 1.13.2 and clang-format 23.1.0. Extracted
Ubuntu Noble development packages and runtime dependencies to a local prefix;
verified Ubuntu's InRelease signature, package-index SHA-256 and each archive's
SHA-256. No system package installation or UID-changing workaround was needed.
Toolchain: GCC 13.3.0, OpenCV 4.6.0, HDF5 1.10.10, spdlog 1.12.0.

Configured `build/issue-394` with Ninja, Release, `BUILD_TESTING=ON`,
`MIB_BUILD_BACKEND_ONLY=ON`, `MIB_ENABLE_MINDVISION=OFF`,
`MIB_ENABLE_HARDWARE_SDKS=OFF`, `MIB_USE_SENTRY=OFF`, and
`CMAKE_DISABLE_FIND_PACKAGE_CURL=ON`. This is an intentional SDK-free
processing/core validation build; no camera hardware or GUI qualification.

Built `mib_backend_tests`, `processing_core_loader_test`,
`processing_core_fixture_matrix_test`, `processing_core_cache_test`,
`processing_core_ed25519_test` and `processing_core_abi_c_test` (including the
native SO and fixture modules).

`ctest --test-dir build/issue-394 -R 'processing\.(science_|core_)'
--output-on-failure -j 2` passes **9/9**: science seam/golden, ABI C, loader,
fixture matrix, Ed25519, cache, activation and activation stress. Formatted
the new C++ ranges with clang-format.

### Windows CI unblock

PR #396's initial Linux backend and sanitizer workflows passed. The Windows
native job failed with `MSB1009: processing_core_authenticode_test.vcxproj`
missing: test-runner consolidation had removed the target still built by
`.github/workflows/python-wheel.yml` and invoked by its signing verifier.
Restored that test to `MIB_STANDALONE_BACKEND_TESTS`. A CMake configure
regression using the actual registration function fails against the old list
and passes against the repaired list. Native Windows build/signing verification
still needs CI; local Linux testing does not substitute for Authenticode.

## Phase 1: shared difference policy (2026-09-10)

`KernelConfig` now names directional subtraction and absolute difference with
`BackgroundDifferenceMode`. Both mask and pre-morphology empty classification
consume the same blur/difference helper. ABI 1 retains its existing flag and
layout; adapters translate it explicitly. Directional subtraction remains the
default, including legacy science processing. Realtime auto-background retains
its explicit absolute comparison. No persisted configuration is migrated.

The loader regression checks independent expected masks for both foreground
polarities and both policies through bundled and native implementations. Blur
and morphology are identity operations in this test to isolate the policy from
legacy ROI-edge morphology. Restoring directional-only mask behavior produces
four failed assertions; restoring the fix passes. Existing numeric and pixel
Contract-1 goldens are unchanged. Processing package version is bumped to 0.2.2
for the corrected absolute-mask behavior; signed release publication is separate.

Linux Release validation: all ten selected science/core tests and realtime
throughput pass, including activation stress, trust, cache and ABI fixtures.
Qt development dependencies have also been provisioned for the next UI phase.
Per user direction, continue implementation and testing locally; Windows
qualification will happen later and does not block the remaining phases.

## Phase 2 progress: catalog diagnostics (2026-09-10)

Replaced generic app/runtime list and activation messages with one structured
catalog evaluation: reason, diagnostic, required and actual values. Covers OS,
architecture, ABI, contract, application bounds, runtime and administrator pin.
Deterministic tests cover each rejection, invalid version bounds, inclusive
maximum and multiple-failure precedence. Qt 6.4.2 catalog executable passes
in the local Linux prefix; the changed dialog translation unit is compiled
separately for syntax validation. This is not a full GUI interaction test.

Phase 2 remains open for structured loader/trust/artifact diagnostics and
future capability negotiation. No loader checks or contract support changed.
