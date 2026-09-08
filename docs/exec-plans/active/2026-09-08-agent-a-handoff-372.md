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
| `claude/host-sdk-reliability-qt-ui-g03ubd` (PR #379 → `develop`) | `9b4f7ada` | **Candidate checkpoint.** Bench-accepted on Windows (this document's evidence section); becomes the accepted SHA when #379 merges. |
| `develop` | `b309061a` | Carries the shared GenTL handle (#378) and the crash-reporter / delivery-mode test fixes; already merged into the candidate. |
| `dev/react-tauri` | `5c7918c6` | Migration baseline Agent B started from; **77 commits behind `develop`**, forked at `c1167f0c`. Its `ExperimentCoordinator` is a different implementation (see §4). |

Nothing below the candidate SHA is accepted. A branch tip is not a checkpoint;
the merge of #379 is.

## 2. Public seams on the candidate (consume as-is)

All headers under `include/backend/`, Qt-free unless stated.

| Capability | Header(s) | Key types / calls | Notes |
|---|---|---|---|
| Capture lifecycle (#365) | `services/CaptureLifecycle.h`, `services/CaptureService.h` | `CaptureLifecycleState`, `CaptureFailureKind`, `CaptureLifecycleSnapshot`, `CaptureStartOutcome {Accepted, AlreadyActive, RejectedStopping, RejectedNoFactory}`; `CaptureService::requestStart()`, `lifecycleSnapshot()`, `waitForState()`, `stop()` | Per-session generation; stale trigger/callback work refused. |
| Readiness + run identity (#369/#274) | `app/ExperimentReadiness.h`, `app/ExperimentCoordinator.h` | `GateStatus`, `ReadinessGate {id, status, reason, remediation, detail}`, `ExperimentReadinessSnapshot {gates, ready, generation}`, `RunConfigurationSnapshot`, `ExperimentStartOutcome {Started, StaleReadiness, NotReady, Failed, AlreadyActive}`; `ExperimentCoordinator::evaluateReadiness(outputPath, profileId)`, `start(ExperimentStartRequest{outputPath, readinessGeneration, acknowledgeLatestFrameDrops})`, `activeRun()`, `finish()`, `reportUnresolvedFault()/clearUnresolvedFault()` | Generation-tagged; `start()` re-evaluates and refuses a stale generation. `finish()` **does not finalize** (see gap G3). |
| Frame accounting (#367) | `recording/RecordingAccounting.h` | `FrameOutcome`, `RecordingAccountingSnapshot`, `reconcile()`, `RunCompletionState {Complete, IntentionallyPartial, IncompleteLoss, Failed, Unknown}` | Equations: `admitted == empty+processed+rejected+processingFailed+storeOverwritten+storeNotCommitted+storeMalformed+cancelledByPolicy+pendingAtStop`; `persistenceAdmitted == committed+failed+pendingAtStop+cancelledByPolicy`. Persisted as `accounting_*` attributes on `/experiment_info` (schema 1). |
| Timestamp semantics (#368) | `camera/common/TimestampValue.h` | `ClockDomain`, `TimestampDescriptor`, checked conversion, wrap detection | HDF5 carries `timestamp_*` attributes (domain, semantic, ticks/s, validity). |
| Telemetry validity (#368) | `services/TelemetrySample.h` | per-metric `validity {valid, stale, unsupported, unknown}`, freshness, generation, sample host time | Unknown is never 0. HDF5 carries `telemetry_<metric>_{value,validity,sample_host_time_us}`. |
| Memory budgets (#370) | `diagnostics/MemoryBudget.h` | owners Measured/Estimated/Unknown, byte budgets, presentation counters | Diagnostics-only surface. |
| Export (#344) | `recording/HdfExportService.h` | `HdfExportRequest`, `HdfExportProgress`, `HdfExportResult {status Completed/Cancelled/Failed}`, `HdfExportCancelToken`, `HdfExportProgressFn`; `newJobId()` | Transactional output, cancellable per phase; 50-run soak evidence in `docs/evidence/2026-09-07-exporter-soak/`. **Not yet reachable through `BackendFacade`** (gap G6). |
| Bridge facade | `app/BackendFacade.h` | `BackendCommand` (camera / recording / processing-settings / recording-load / playback-seek), `BackendEvent` (frame-ready, camera-status, recording-status, processing-result, playback-position, error), `fetchLatestFrame`, `fetchFrameByIndex`, `BackendFrame` | Uses Qt types nowhere. Has **no experiment, readiness, config or export commands** (gaps G2, G4, G6). |

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
| G2 | Readiness / start / frozen-run types are not exposed through `BackendFacade`; only the Qt `MainWindow` calls the coordinator. | Tauri Start stays disabled. | Add `ExperimentCommand {EvaluateReadiness, Start, Stop}` + `ExperimentStatusEvent` to the facade, delegating to the same `ExperimentCoordinator`. |
| G3 | **Finalization is owned by `MainWindow::finishStopExperiment`** (flush, accounting write, HDF5 close). `ExperimentCoordinator::finish()` leaves it to the caller. Bench evidence of the cost (2026-09-08): the window-owned final append bypassed the write-queue accounting and labelled a clean run IntentionallyPartial, and a modal held the file open for 108 s; both fixed in the window, but the ownership is still wrong. The migration coordinator on `dev/react-tauri` instead owns a worker with periodic flush + async stop. | Blocks M4 ("no Qt timer may be required"). Merging `dev/react-tauri` into `develop` will conflict on `ExperimentCoordinator.{h,cpp}` because both add the file. | Converge under Agent A: move the finalize sequence from `MainWindow` into the reliability coordinator (`stop()` → async finalize → terminal `RunCompletionState`), then delete the migration coordinator during the merge. Neither file may be taken wholesale. |
| G4 | Configuration application/persistence is Qt-side (`AppConfigWatcher`); no Qt-free checked-patch seam with baseline revision and saved/applied/verified/conflict outcomes. | M3/M4 config workflow. | Extract the document/transaction core of `AppConfigWatcher` behind a Qt-free interface; Qt watcher becomes one client. |
| G5 | No authoritative session+snapshot watermark or retained operation outcomes with expiry. | M2 recovery after missed events. | New facade query `snapshot()` returning session generation, revision, current faults, recent operation outcomes (bounded). |
| G6 | `HdfExportService` exists and is soaked, but has no facade operation binding (submit/progress/cancel). | M4 step 7. | Add `ExportCommand` + progress/terminal events to the facade over the existing service; no new engine. |
| G7 | `dev/react-tauri` is 77 commits behind `develop`; trial merge of today's `develop` (without the coordinator) into Agent B's tip conflicts in 18 files (`CameraControlService.cpp`, `CrashReporter.cpp`, `SyringePumpService.{h,cpp}`, `TriggerService.cpp`, `MindVisionConfig.h`, `MockCamera.h`, CI yml, vault). | Every one of these is a reliability or camera fix that must win. | Agent B rebases its stack onto `develop` after #379 merges; Agent A reviews the six backend conflicts. |
| G8 | The Rust bridge only built on Linux (`build.rs` hard-coded the `linux-backend-only` preset and Ubuntu library paths). | M4 requires native Windows evidence. | **Done locally** on `agent-b/windows-native-bridge` (this bench, not pushed): CMake-derived link manifest (`tools/gen_bridge_link_manifest.py`) + Windows path in `build.rs`; bridge contract suite 16/16 and desktop crate 16/16 pass natively on Windows. Also found there: cxx-build drops the C++ wrappers of `#[cfg(feature)]` bridge functions (the red desktop CI on PR #375), the MSVC CRT never sees `MIB_BRIDGE_MAX_QUEUE` set by the Rust test host, and the autofocus test assumed COM3 does not exist. |

## 5. Tests that guard the seams (all green on the candidate, Windows Release)

`backend.capture_lifecycle`, `backend.trigger_session`, `backend.experiment_readiness`, `backend.frame_store_commit`, `processing.experiment_accounting`, `recording.accounting`, `backend.timestamp_telemetry`, `processing.memory_budget`, `recording.hdf_export_service`, `recording.hdf_export_soak`, `scripts.exporter_soak`, `backend.facade_boundary`, `hardware.discovery_reentry`, plus the frontend lane (`frontend.*`, 19) and integration lane (10).

## 6. Bench evidence for the candidate (Windows 11, Coaxlink Quad CXP-12 / EoSens 2.0MCX12)

- Fast lane 94/94, integration 10/10, frontend 19/19, hardware 5/5 (pump absent, skipped); `check_docs.py` OK.
- Two real experiments on hardware at 5000 fps: run 1 admitted 3,211,240 frames, run 2 admitted 756,687; both reconciled independently from the HDF5 (`accounting_*` attributes re-summed by a separate script: frame terms and persistence terms match, derived state Complete equals stored). Both were all-empty runs (no sample flow), so persistence terms were exercised only as zero; a mock-camera run on the Hugging Face `gavinlouuu/512x96stream` frames covers the non-zero persistence path (see the bench report).
- Telemetry stored with validity: 88 SDK buffer underruns recorded, transport loss `unsupported`, sequence gaps 0.
- 3-minute capture soak: threads 97→91, handles 730→714, working set plateau 10.2 GB (5000-slot FrameStore), clean mid-capture close, no crash artifacts.

Bench reports: beta `fa6e6ba` validation and reliability-branch acceptance
(Claude Code artifacts linked from PR #379).

## 7. Rules for consuming this handoff

- Consume the types in §2 by value; do not re-declare them in Rust/TypeScript beyond the generated contract.
- Gaps G1–G6 are Agent A deliverables; Agent B may build fixture-only UI against them but must not ship a production fallback.
- No `Closes` references to #371/#372/#246/#304 from any PR that implements part of this.
