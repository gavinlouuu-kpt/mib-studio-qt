# Reliability release evidence (epic #371)

Branch `claude/host-sdk-reliability-qt-ui-g03ubd` (target `develop`),
commits `788ae83 … 62ca332`, 2026-09-06 → 2026-09-07. Every gate below was
implemented in the phase order the epic prescribes, each phase in its own
commit with its vault notes, and verified on the Linux backend-only and
system-Qt (6.4.2, offscreen) builds. All counts are from the final run on
the last commit unless stated.

| Lane | Command | Result |
|---|---|---|
| Backend + scripts | `ctest --test-dir build/linux-backend -E performance\.` | 99 tests, 0 failed (4 hardware tests skipped — no devices) |
| Frontend (Qt, offscreen) | `ctest --test-dir build/linux-system -R '^frontend\.'` | 19 tests, 0 failed |
| ThreadSanitizer (`-DMIB_SANITIZER=thread`) | `backend.experiment_readiness`, `recording.hdf_export_service`, `processing.memory_budget`, `processing.experiment_accounting`, `backend.processing_batch_pipeline`, `backend.frame_store_concurrency` | no reports (suppressions: `tests/sanitizer/tsan.supp`) |
| Screenshot tour | `screenshot_tour --out docs/manual/images` | 9 shots, geometry assertions pass, `check_screenshots` in sync |
| Vault/docs | `scripts/check_docs.py` | OK |

## Release completion criteria

| Criterion (epic) | Issue | Implementation | Deterministic guard(s) | Notes |
|---|---|---|---|---|
| No camera/SDK/trigger object outlives its session or is reused by stale work | #365 | `CaptureLifecycle` state machine + per-session generation, `requestStart()` typed outcome, faulted-worker reaping, `TriggerService::setCamera(cam, gen)` waits for in-flight pulses and refuses stale requests, shutdown order | `backend.capture_lifecycle` (blocked grab, slow teardown, start failure, restart after fault, 120-cycle stress), `backend.trigger_session`, `backend.lifecycle_smoke` | |
| Camera conversion fails closed on unverified format/geometry | #366 | MindVision `SdkOps` seam: Mono8 set + read-back, checked geometry, per-frame header validation → structured fault, bounded in-flight wait before `CameraUnInit` | `backend.mindvision_conversion_fault`, `backend.mindvision_config`, `backend.mindvision_selection_state` | Real-SDK behaviour on Windows hardware not exercised here (see Open) |
| One guarded camera command path in the UI | #360 | `CameraController` single dispatch, operation guard, every button/shortcut a presentation of the same actions | `frontend.camera_action_state`, `frontend.ui_layout` | |
| Empty / scientific rejection / processing failure / store loss / persistence failure are distinct and reconciled; a run with unexplained loss cannot be Complete | #367 | `RecordingAccounting` (`FrameOutcome`, `reconcile()` → `RunCompletionState`), FrameStore reserve→commit boundary + typed reads, `classifyFrameWithActiveKernel`, accounting persisted to HDF5 (`accounting_*`) and shown in Review | `backend.frame_store_commit`, `processing.experiment_accounting`, `recording.accounting`, `processing.fault_injection` | |
| Timestamp units/clock domains and per-metric telemetry validity survive to HDF5/UI/Review; unknown never shown as 0 | #368 | `TimestampValue.h` (`ClockDomain`, `TimestampDescriptor`, checked conversion, wrap detection), `TelemetrySample.h` per-metric validity/freshness/generation, acquisition provenance in HDF5, `formatMetric` (`n/a` / `unsupported` / stale) | `backend.timestamp_telemetry`, `frontend.run_status_ui` (unknown never rendered as 0) | |
| Start uses current backend readiness and freezes immutable provenance; reconnect/config/background/core changes invalidate readiness; mock is explicit | #369, #274 | `ExperimentCoordinator`: 16 gates, generation-tagged readiness, serialized `start()` refusing stale generations, frozen `RunConfigurationSnapshot` in `/run_provenance`, `finish()`, unresolved-fault latch; `cameraSourceInfo()` requested vs effective source | `backend.experiment_readiness` (+TSan) | |
| Background calibration is finite/cancellable and preserves the previous background | #369 | `startBackgroundCalibration` (required/attempts/timeout/cancel, contamination counted, atomic publish) | `backend.experiment_readiness` (calibration cases) | |
| Exporter passes the 50-run memory/timing/thread lifecycle gate | #344 | Streaming `hdf_export_engine`, deterministic worker lifecycle, native transactional `HdfExportService`, async Review exports | `scripts.export_hdf5_streaming`, `scripts.hdf5_export_app_lifecycle`, `recording.hdf_export_service` (+TSan); gates `scripts.exporter_soak`, `recording.hdf_export_soak` | `docs/evidence/2026-09-07-exporter-soak/` (GUI 50 cycles RSS 124.0→124.3 MB, engine 50, native 50) |
| Persistent run state / alerts render failures truthfully; persistence never coupled to UI visibility | #363 (+#358 #359 #361 #362 #364) | `RunStatusModel` (operation ids, latched failure), `UiAlertModel` + banner, bounded status metrics, Diagnostics dialog, two-phase async stop; viewport-safe layout, single-owner sidebar, explicit config edit state, acknowledged tune apply | `frontend.run_status_model`, `frontend.run_status_ui` (25 metric ticks never erase an alert; latched Failed survives Complete), `frontend.window_geometry_policy`, `frontend.ui_layout`, `frontend.config_document_state`, `frontend.config_tabs_state`, `frontend.preview_layout`, `frontend.config_draft`, `frontend.monitoring_tune`, `frontend.config_apply` | Monitoring rings gated by tab visibility only affect presentation; recording accounting is independent (`processing.memory_budget` realtime case) |
| Major host queues/retained images have bounded byte policies; presentation stalls cannot alter recording | #370 | `MemoryBudget.h` owners (Measured/Estimated/Unknown), `ExperimentFrameBuffer` (frames + bytes), batch-queue byte budget, FrameStore measured retention, per-object sharing, presentation counters, Diagnostics memory section | `processing.memory_budget` (+TSan), `performance.memory_budget` | `docs/evidence/2026-09-07-memory-budget/` |
| Contract-v1 science unchanged by the refactors | — | Science untouched; sharing/ownership changes verified against identical metrics | `processing.science_golden`, `processing.science_seam`, `processing.core_fixture_matrix`, `scripts.run_processing_conformance_input`, `processing.memory_budget` (identical results with shared images) | Contract-v2 (#296/#303) stays a separate versioned change |
| MindVision, EGrabber, mock, EveryFrame/LatestFrame, HDF5, Review/export, no-hardware startup regressions pass | all | — | `camera.mock_smoke`, `camera.delivery_mode_contract`, `camera.delivery_mode_overload`, `recording.lifecycle`, `recording.hdf5_resilience`, `recording.experiment_roundtrip`, `integration.e2e_storage_destinations`, `backend.smoke`, `frontend.ui_layout` (no-hardware startup), screenshot tour | Hardware-backed MindVision/EGrabber acceptance: see Open |

## Required evidence checklist

- [x] delayed/failing fake camera and trigger lifecycle — `backend.capture_lifecycle`, `backend.trigger_session`
- [x] Mono8/geometry conversion fault injection — `backend.mindvision_conversion_fault`
- [x] FrameStore overwrite/not-yet-committed and processing failure accounting — `backend.frame_store_commit`, `processing.experiment_accounting`, `recording.accounting`
- [x] slow/full/failing persistence and finalization — `recording.accounting` (failed writer), `backend.hdf_write_queue`, `recording.hdf5_resilience`, `frontend.run_status_ui` (save failures latch Failed)
- [x] timestamp unit/validity/legacy fixtures — `backend.timestamp_telemetry`
- [x] stale readiness/reconnect/config/background changes — `backend.experiment_readiness`
- [x] background contamination/timeout/cancellation — `backend.experiment_readiness`
- [x] exporter cancellation/close/partial-output — `scripts.hdf5_export_app_lifecycle`, `scripts.export_hdf5_streaming`, `recording.hdf_export_service`
- [x] mock experiment start → accumulate → stop → reopen/reconcile — `recording.experiment_roundtrip`, `recording.accounting`, `backend.experiment_readiness`
- [x] sanitizer/concurrency lanes — TSan runs above; `.github/workflows/sanitizers.yml` (thread, address+undefined, nightly + on demand)
- [x] 50 consecutive exporter runs, bounded RSS, stable timing — exporter soak evidence
- [x] repeated camera start/stop/fault/restart/shutdown stress — `backend.capture_lifecycle` (120 cycles), `backend.lifecycle_smoke`
- [x] bounded memory/queue high-water trends — memory-budget evidence; `.github/workflows/soak.yml` now also runs `performance.memory_budget`
- [x] reopen Review/export and verify recorded provenance/accounting — `recording.experiment_roundtrip`, `recording.accounting`, `backend.experiment_readiness` (run snapshot round trip)
- [ ] long host-path acquisition/recording soak with exact final accounting on hardware — not possible in this environment (mock only); the accounting equations are enforced on every mock run
- [ ] supported MindVision/EGrabber hardware acceptance — requires the Windows hardware bench (`build-windows.yml` + `hardware.*` tests, currently skipped without devices)
- [ ] bounded thread/file-handle trends on Windows — RSS helper is Windows-first; Linux runs use `/proc/self/status`

## Open items carried out of the release

1. Hardware acceptance (MindVision, EGrabber) and the packaged Windows
   executable: every guard is deterministic and hardware-free; the
   `hardware.*` tests and the Windows workflow must be run on the bench
   before tagging.
2. FPGA Direct (#349) and the React/Tauri shell (#246) are untouched by this
   release, per the epic's scope.
3. Contract-v2 (#296/#303) remains a separately versioned scientific change.
