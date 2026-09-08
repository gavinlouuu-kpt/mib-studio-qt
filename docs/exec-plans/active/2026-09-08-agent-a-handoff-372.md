# Agent A → Agent B handoff manifest (issue #372, epic #371)

Status: active

Date: 2026-09-08. Owner: Agent A (reliability backend). Consumer: Agent B
(React/Tauri bridge). Requested on #372 (comment 5565220578): "a committed
handoff containing the exact backend SHA, public types/commands,
operation/cancellation rules, timestamp/validity semantics, config/readiness
revisions, relevant tests, and known gaps". This is that document. It names
what is accepted, what is only a candidate, and what does not exist yet.

## 1. Backend revision

| Ref | SHA | Status |
|---|---|---|
| `claude/host-sdk-reliability-qt-ui-g03ubd` (PR #379 → `develop`) | `c48f5bf2` | **Candidate checkpoint.** Bench-accepted on Windows (this document's evidence section); becomes the accepted SHA when #379 merges. Supersedes `9b4f7ada` with the shared-backend experiment lifecycle (G2/G3 closed) and the accounting boundary fix. |
| `develop` | `b309061a` | Carries the shared GenTL handle (#378) and the crash-reporter / delivery-mode test fixes; already merged into the candidate. |
| `dev/react-tauri` | `5c7918c6` | Migration baseline Agent B started from; **77 commits behind `develop`**, forked at `c1167f0c`. Its `ExperimentCoordinator` is a different implementation (see §4). |

Nothing below the candidate SHA is accepted. A branch tip is not a checkpoint;
the merge of #379 is.

## 2. Public seams on the candidate (consume as-is)

All headers under `include/backend/`, Qt-free unless stated.

| Capability | Header(s) | Key types / calls | Notes |
|---|---|---|---|
| Capture lifecycle (#365) | `services/CaptureLifecycle.h`, `services/CaptureService.h` | `CaptureLifecycleState`, `CaptureFailureKind`, `CaptureLifecycleSnapshot`, `CaptureStartOutcome {Accepted, AlreadyActive, RejectedStopping, RejectedNoFactory}`; `CaptureService::requestStart()`, `lifecycleSnapshot()`, `waitForState()`, `stop()` | Per-session generation; stale trigger/callback work refused. |
| Readiness + run identity (#369/#274) | `app/ExperimentReadiness.h`, `app/ExperimentCoordinator.h` | `GateStatus`, `ReadinessGate {id, status, reason, remediation, detail}`, `ExperimentReadinessSnapshot {gates, ready, generation}`, `RunConfigurationSnapshot`, `ExperimentStartOutcome {Started, NotReady, StaleReadiness, AlreadyActive, StorageFailed, ProvenanceFailed, Busy}`; `ExperimentCoordinator::evaluateReadiness(outputPath, profileId)`, `start(ExperimentStartRequest{outputPath, readinessGeneration, profileId, acknowledgeLatestFrameDrops})`, `activeRun()`, `reportUnresolvedFault()/clearUnresolvedFault()` | Generation-tagged; `start()` re-evaluates and refuses a stale generation. |
| Experiment lifecycle (#372 G2/G3) | `app/ExperimentReadiness.h`, `app/ExperimentCoordinator.h` | `ExperimentRunState {Idle=0, Starting=1, Active=2, Stopping=3, Failed=4}` (= contract `experiment_states`), `ExperimentStopOutcome {Accepted, NotActive, Busy}`, `ExperimentStatus {state, start/readiness/capture generations, outputPath, start/end wall-clock ns, buffered counts, persistence admitted/committed/failed, flushing, cancelled, terminal, finalizationOk, completion (RunCompletionState), completionReason, faultCode/faultMessage, message}`; `requestStop(cancelled)`, `status()`, `setStatusCallback(cb)`, `onFatalSaveError(msg)`, `shutdown()` | The coordinator owns periodic flush, Stop, finalization (drain, remainder, experiment info, accounting, acquisition provenance, config JSON, close) and the terminal outcome on its own worker; no Qt object is required for a run. Callback fires outside the mutex; consumers must not block. Terminal status stays readable while Idle until the next Start. Spec: `docs/superpowers/specs/2026-09-08-shared-backend-experiment-lifecycle-design.md`. |
| Frame accounting (#367) | `recording/RecordingAccounting.h` | `FrameOutcome`, `RecordingAccountingSnapshot`, `reconcile()`, `RunCompletionState {Complete, IntentionallyPartial, IncompleteLoss, Failed, Unknown}` | Equations: `admitted == empty+processed+rejected+processingFailed+storeOverwritten+storeNotCommitted+storeMalformed+cancelledByPolicy+pendingAtStop`; `persistenceAdmitted == committed+failed+pendingAtStop+cancelledByPolicy`. Persisted as `accounting_*` attributes on `/experiment_info` (schema 1). |
| Timestamp semantics (#368) | `camera/common/TimestampValue.h` | `ClockDomain`, `TimestampDescriptor`, checked conversion, wrap detection | HDF5 carries `timestamp_*` attributes (domain, semantic, ticks/s, validity). |
| Telemetry validity (#368) | `services/TelemetrySample.h` | per-metric `validity {valid, stale, unsupported, unknown}`, freshness, generation, sample host time | Unknown is never 0. HDF5 carries `telemetry_<metric>_{value,validity,sample_host_time_us}`. |
| Memory budgets (#370) | `diagnostics/MemoryBudget.h` | owners Measured/Estimated/Unknown, byte budgets, presentation counters | Diagnostics-only surface. |
| Export (#344) | `recording/HdfExportService.h` | `HdfExportRequest`, `HdfExportProgress`, `HdfExportResult {status Completed/Cancelled/Failed}`, `HdfExportCancelToken`, `HdfExportProgressFn`; `newJobId()` | Transactional output, cancellable per phase; 50-run soak evidence in `docs/evidence/2026-09-07-exporter-soak/`. **Not yet reachable through `BackendFacade`** (gap G6). |
| Bridge facade | `app/BackendFacade.h` | `BackendCommand` (camera / recording / processing-settings / recording-load / playback-seek / **experiment**), `BackendEvent` (frame-ready, camera-status, recording-status, processing-result, playback-position, error, **experiment-status**), `fetchLatestFrame`, `fetchFrameByIndex`, `fetchExperimentReadiness(out, outputPath, profileId)`, `fetchExperimentStatus(out)`, `BackendFrame`. `BackendCommandType` values are explicit and contract-pinned (`Experiment = 6`; 5 reserved for `Operation`). `ExperimentCommand {action: EvaluateReadiness=0 / Start=1 / Stop=2 / Status=3, outputPath, readinessGeneration, profileId, acknowledgeLatestFrameDrops, cancelled}`; `BackendCommandResult.experimentStartOutcome / experimentStopOutcome` (typed). `ExperimentStatusEvent {status}` is the variant's last alternative (index 6 here; the bridge branch places `OperationStatusEvent` at 6 and it at 7, so the merge must keep the bridge order). | Uses Qt types nowhere. Has **no config or export commands** (gaps G4, G6). |

## 3. Semantics Agent B may rely on

- **Acceptance ≠ completion.** `requestStart()` and `ExperimentCoordinator::start()` return typed outcomes; a timeout on the caller side means "unknown, query `lifecycleSnapshot()` / `activeRun()`", never "stopped".
- **Generations.** Capture session generation (`CaptureLifecycleSnapshot.generation`) and readiness generation (`ExperimentReadinessSnapshot.generation`) are monotonic per process; anything tagged with an older generation is stale and is refused by the backend, not by the UI.
- **Duplicate start** while active → `AlreadyActive`; during a stop → `RejectedStopping`. Never two sessions.
- **Faults latch.** `reportUnresolvedFault` blocks readiness (`lifecycle.fault` gate) until `clearUnresolvedFault`; acknowledgement is explicit.
- **Accounting is authoritative and view-independent**: it is computed in `ProcessingService`/`RecordingAccounting`, not in any widget. Verified on the bench with hidden panels (§6).
- **Unknown telemetry is unknown**, not zero; unsupported metrics say so (`telemetry_transport_lost_frames_validity = unsupported` on EGrabber).

## 4. Known gaps (owner: Agent A unless stated)

| # | Gap | Effect on #372 | Plan |
|---|---|---|---|
| G1 | `BackendFrame` has no source/session/config identity and no `TimestampDescriptor`; packet v1 marks them unavailable. | M1/M4 overlay-identity and source-switch acceptance stay open. | Additive fields on `BackendFrame` from `CaptureLifecycleSnapshot.generation` + config revision; no ABI break. |
| G2 | ~~Readiness / start / frozen-run types are not exposed through `BackendFacade`.~~ **Closed** (`b0d2497a`): `ExperimentCommand`, `fetchExperimentReadiness/Status`, typed start/stop outcomes, `ExperimentStatusEvent`. | Tauri Start can be enabled once the bridge maps its `experiment_*` functions onto `ExperimentCommand` (contract PR on the bridge branch). | Contract PR: append `experiment_command_actions`, `experiment_start_outcomes`, `experiment_stop_outcomes`, `run_completion_states`, `readiness_gate_statuses` (ABI 13); typed `ExperimentStatus` JSON payload with decimal-string u64 identities. |
| G3 | ~~Finalization is owned by `MainWindow::finishStopExperiment`.~~ **Closed** (`6630c851`, `83af2a28`): `ExperimentCoordinator` owns periodic flush, `requestStop()` → worker finalization → terminal `ExperimentStatus` (completion from the reconciled accounting), `onFatalSaveError()` → `Failed` with a readable file, bounded `shutdown()` first in `AppBackend::shutdown()`. `MainWindow` is a client (`requestStop` + `onExperimentStatus`); it no longer touches `Hdf5Service` for a run. | Merging `dev/react-tauri` into `develop` still conflicts on `ExperimentCoordinator.{h,cpp}` (both add the file). | At the merge, take the reliability coordinator wholesale and delete the migration one; port the migration facade's remaining command handlers onto the reliability `BackendFacade` shape (see the seams table for the event-variant order). |
| G4 | Configuration application/persistence is Qt-side (`AppConfigWatcher`); no Qt-free checked-patch seam with baseline revision and saved/applied/verified/conflict outcomes. | M3/M4 config workflow. | Extract the document/transaction core of `AppConfigWatcher` behind a Qt-free interface; Qt watcher becomes one client. |
| G5 | No authoritative session+snapshot watermark or retained operation outcomes with expiry. **Partly served**: `fetchExperimentStatus()` returns the last run's terminal outcome (completion, fault) until the next Start, so a client that missed the `ExperimentStatusEvent` can still recover the experiment outcome. Session watermark, config revision and retained operation outcomes remain open. | M2 recovery after missed events. | New facade query `snapshot()` returning session generation, revision, current faults, recent operation outcomes (bounded). |
| G6 | `HdfExportService` exists and is soaked, but has no facade operation binding (submit/progress/cancel). | M4 step 7. | Add `ExportCommand` + progress/terminal events to the facade over the existing service; no new engine. |
| G7 | `dev/react-tauri` is 77 commits behind `develop`; trial merge of today's `develop` (without the coordinator) into Agent B's tip conflicts in 18 files (`CameraControlService.cpp`, `CrashReporter.cpp`, `SyringePumpService.{h,cpp}`, `TriggerService.cpp`, `MindVisionConfig.h`, `MockCamera.h`, CI yml, vault). | Every one of these is a reliability or camera fix that must win. | Agent B rebases its stack onto `develop` after #379 merges; Agent A reviews the six backend conflicts. |
| G8 | The Rust bridge only built on Linux (`build.rs` hard-coded the `linux-backend-only` preset and Ubuntu library paths). | M4 requires native Windows evidence. | **Done locally** on `agent-b/windows-native-bridge` (this bench, not pushed): CMake-derived link manifest (`tools/gen_bridge_link_manifest.py`) + Windows path in `build.rs`; bridge contract suite 16/16 and desktop crate 16/16 pass natively on Windows. Also found there: cxx-build drops the C++ wrappers of `#[cfg(feature)]` bridge functions (the red desktop CI on PR #375), the MSVC CRT never sees `MIB_BRIDGE_MAX_QUEUE` set by the Rust test host, and the autofocus test assumed COM3 does not exist. |

## 5. Tests that guard the seams (all green on the candidate, Windows Release)

`backend.capture_lifecycle`, `backend.trigger_session`, `backend.experiment_readiness` (now also: `requestStop` → terminal Complete with the stop-time remainder committed, fatal save error → `Failed` with a readable closed file, `Busy`/`NotActive`, shutdown while Active, Starting/Active/Stopping/Idle callback sequence), `backend.frame_store_commit`, `processing.experiment_accounting` (now also experiment 3: 40 short runs against a free-running pusher, no start/stop boundary skew), `recording.accounting`, `backend.timestamp_telemetry`, `processing.memory_budget`, `recording.hdf_export_service`, `recording.hdf_export_soak`, `scripts.exporter_soak`, `backend.facade_boundary` (now also: readiness pull → Start → AlreadyActive → Stop accepted → terminal Idle → NotActive, with the `ExperimentStatusEvent` sequence), `hardware.discovery_reentry`, plus the frontend lane (`frontend.*`, 19) and integration lane (10).

Found by these tests on 2026-09-08 and fixed in `c48f5bf2`: outcomes were counted "while the experiment is active" instead of "for admitted frames", so a frame in flight across `startExperiment()`/`endExperiment()` skewed the frame terms by one and labelled a clean run `Failed: accounting does not reconcile` (hit once by the CI fast lane and once by a bench run within an hour). Outcomes, validations and the buffer append are now gated on the run's admitted index range; `endExperiment()` waits (≤ 250 ms) for the last admitted frame.

## 6. Bench evidence for the candidate (Windows 11, Coaxlink Quad CXP-12 / EoSens 2.0MCX12)

- Fast lane 94/94, integration 10/10, frontend 19/19, hardware 5/5 (pump absent, skipped); `check_docs.py` OK. Re-run on `c48f5bf2` (shared-backend lifecycle): fast 94/94, integration 10/10, frontend 19/19 + 4 lifecycle tests, hardware 4 passed / pump skipped.
- Shared-backend lifecycle through the Qt window (mock camera on the Hugging Face `gavinlouuu/512x96stream` frames, 1 ms interval, 2 min): `requestStop` → coordinator finalization 46 ms (final flush 384 frames 43 ms, close 0.2 ms), 384/384 persisted, 0 pending at stop, frame terms 76,726 = 76,726 (reconciled independently from the HDF5), the accounting dialog shown only after the close. Completion `IncompleteLoss` (15 ring overwrites, 43,664 sequence gaps) is the mock stream's throughput at 1 ms, identical to the pre-change run on the same stream (11 / 34,036), not a lifecycle regression. Files: `D:\data\bench-validation\shared-backend-run{,2}.h5`.
- Two real experiments on hardware at 5000 fps: run 1 admitted 3,211,240 frames, run 2 admitted 756,687; both reconciled independently from the HDF5 (`accounting_*` attributes re-summed by a separate script: frame terms and persistence terms match, derived state Complete equals stored). Both were all-empty runs (no sample flow), so persistence terms were exercised only as zero; a mock-camera run on the Hugging Face `gavinlouuu/512x96stream` frames covers the non-zero persistence path (see the bench report).
- Telemetry stored with validity: 88 SDK buffer underruns recorded, transport loss `unsupported`, sequence gaps 0.
- 3-minute capture soak: threads 97→91, handles 730→714, working set plateau 10.2 GB (5000-slot FrameStore), clean mid-capture close, no crash artifacts.

Bench reports: beta `fa6e6ba` validation and reliability-branch acceptance
(Claude Code artifacts linked from PR #379).

## 7. Rules for consuming this handoff

- Consume the types in §2 by value; do not re-declare them in Rust/TypeScript beyond the generated contract.
- Gaps G1–G6 are Agent A deliverables; Agent B may build fixture-only UI against them but must not ship a production fallback.
- No `Closes` references to #371/#372/#246/#304 from any PR that implements part of this.
