# ExperimentCoordinator

> Backend-owned **experiment readiness transaction** and **immutable run
> snapshot** (issue #369; host-SDK portion of #274). The only thing that can
> authorize an experiment Start is a readiness evaluation performed by the
> backend against its *actual* current state — and that authorization is
> generation-tagged, so a stale preflight never starts a run. Since the
> shared-backend lifecycle work (issue #372 G2/G3) it also owns the run
> after Start: periodic flush, Stop, finalization and the terminal
> accounting outcome, so the Qt window and the bridge are two clients of
> one object and neither touches `Hdf5Service` for a run.

**Source:** `src/backend/app/ExperimentCoordinator.cpp`,
`include/backend/app/ExperimentCoordinator.h`,
`include/backend/app/ExperimentReadiness.h` (Qt-free types + JSON serializers)
**Tests:** `tests/backend/experiment_readiness_test.cpp`
(`backend.experiment_readiness`, normal + TSan)
**Related:** [[AppBackend]], [[../services/CaptureService]],
[[../services/ProcessingService]], [[../services/Hdf5Service]],
[[../data-model/HDF5-Storage]], [[../frontend/MainWindow]]

## Responsibility

- `evaluateReadiness(outputPath, profileId)` returns an
  `ExperimentReadinessSnapshot`: a list of `ReadinessGate`s (`id`, `status`,
  `reason`, `remediation`, `detail`), `ready` (no gate blocks), a
  **generation**, and the `candidate` `RunConfigurationSnapshot` the
  evaluation was based on. `GateStatus` is `Pass / Warn / Fail / Unavailable /
  NotRequired`; `Fail` and `Unavailable` block — **unknown is never Pass**.
- The generation increments only when an *invalidation input* changes
  (`InvalidationKey`: capture generation + readiness, effective camera source
  + fallback flag, active delivery mode, processing config version, raw
  `config.json` sha, core version/sha/pin, background generation, ROI,
  pixel-to-micron factor, output path, profile id, unresolved fault). A stable
  state keeps its generation, so a preflight stays usable until something
  actually changes.
- `start(ExperimentStartRequest{outputPath, readinessGeneration, profileId,
  acknowledgeLatestFrameDrops})` is the serialized Start transaction:
  1. `try_lock` — a concurrent transaction gets `Busy`; a run in progress
     gets `AlreadyActive`.
  2. Re-evaluate now; `readinessGeneration` mismatch → `StaleReadiness`
     (reconnect, config/background/core/ROI/output change, new fault since
     the presented preflight). Not ready → `NotReady`. LatestFrame without
     acknowledgement → `NotReady`.
  3. Freeze the `RunConfigurationSnapshot` from the evaluated candidate
     (start generation, host + wall-clock start time, output path).
  4. Open the HDF5 file + `initializeDatasets()` (`StorageFailed` rolls back
     to Idle), then persist the snapshot **first** via
     `Hdf5Service::writeRunSnapshotJson` (`ProvenanceFailed` closes and
     removes the file). A run without its frozen snapshot is never Running.
  5. `setExperimentAccountingContext(captureGeneration, latestFrame)` +
     `ProcessingService::startExperiment()`; start the worker thread (first
     run only); publish `Active`. Before step 2 a multi-image series run
     switches the realtime mode `AsyncBatch -> Inline` (so the frozen
     snapshot records the mode actually used); the finalization restores it.
- `requestStop(cancelled)` returns `ExperimentStopOutcome::Accepted` and
  hands the finalization to the worker; `Busy` while Starting/Stopping or a
  stop is already queued; `NotActive` when there is no run. Completion is
  observed through `status()` / the status callback (`terminal == true`).
- `onFatalSaveError(message)` (wired from
  `ProcessingService::setFlushErrorCallback` by [[AppBackend]]) marks the run
  `Failed` and runs the same finalization so the file is closed and readable.
- `shutdown()` (from `AppBackend::shutdown()` and the destructor) finalizes
  an active run and joins the worker; idempotent; after it `start()` returns
  `Busy`.
- `AppBackend::initialize()` supplies default application identity for every shell
  (#545): CMake `PROJECT_VERSION_FULL` (fallback `PROJECT_VERSION`), configure-time
  `MIB_BUILD_ID` environment value (fallback `dev`), and platform plus architecture.
  `setApplicationIdentity()` still overrides these defaults for subsequent runs;
  an active run's frozen snapshot remains unchanged. Qt retains its explicit identity.
- `finish()` no longer changes state: it returns the active (or most recently
  finalized) run snapshot for callers that log the identity.
- `status()` / `setStatusCallback(cb)`: `ExperimentStatus` (state,
  generations, output path, start/end wall-clock, live buffered counts,
  successful valid/invalid saved counts, valid/invalid policy drops,
  persistence admitted/committed/failed, `flushing`, `cancelled`,
  `terminal`, `finalizationOk`, `completion` + reason, fault code/message,
  message). The callback fires on every transition **outside the mutex** and
  consumers must not block (same rule as the facade event sink). The
  terminal status of the last run stays readable while Idle until the next
  start resets it.
- `reportUnresolvedFault(code, message)` / `clearUnresolvedFault()`: a save
  or provenance failure from the last run blocks the next Start
  (`lifecycle.fault` gate) until the operator acknowledges it;
  `clearUnresolvedFault()` also moves a `Failed` coordinator back to `Idle`.

## Run states

`ExperimentRunState` lives in `ExperimentReadiness.h` with the bridge
contract's values (`experiment_states`): `Idle=0, Starting=1, Active=2,
Stopping=3, Failed=4` (append only). `Failed` is the resting state after a
failed finalization (fault latched); `Idle` after a clean one. The other
enums the bridge contract pins carry explicit values too (ABI 13):
`ExperimentStartOutcome` (`experiment_start_outcomes`, `Started=0 … Busy=6`),
`ExperimentStopOutcome` (`experiment_stop_outcomes`), `GateStatus`
(`readiness_gate_statuses`, `Pass=0 … NotRequired=4`) and
`RunCompletionState` (`run_completion_states`, `Complete=0 … Unknown=4`);
append only, never renumber.

## Finalization sequence (worker, on `requestStop` / fatal / shutdown)

1. Publish `Stopping` (or `Failed` for a fatal save error).
2. `flushBufferedFrames(hdf5)` + `finishFlush()`: drain the async write
   queue; the writer thread has stopped afterwards.
3. `endExperiment()`; `resetRealtimeMetrics()`.
4. Remainder that arrived between 2 and 3 goes through `flushBufferedFrames`
   + `finishFlush` again so `persistenceCommitted` credits it (a direct
   `appendFrames` left a clean run labelled IntentionallyPartial; bench,
   2026-09-08).
5. `Hdf5Service::flush()`; `writeExperimentInfo(...)` (start/end wall-clock,
   run-wide successful valid/invalid writes, processing config, ROI, background, core identity);
   `writeRunAccounting(experimentAccountingSnapshot())`;
   `writeAcquisitionProvenance(...)`; `writeConfigJson(getLastConfigJson())`;
   then, best effort, `writeKdeLiveJson(...)` with the last provisional KDE
   core record [[../services/MonitoringDensityService]] handed over through
   `setLiveKdeCoreRecord` (from its backend worker, no shell involved)
   (accepted only while `Active`, cleared at Start; a failed write logs a
   warning and never changes the run outcome).
6. `closeFile()`.
7. Restore the realtime mode if Start switched it.
8. Terminal status: `terminal=true`, `completion` from the reconciled
   accounting (`Failed` for a fatal save error), `finalizationOk=false` if
   2/4/5/6 failed, in which case the unresolved fault
   `experiment.flushFailed` / `experiment.provenanceFailed` /
   `experiment.saveFailed` is latched and the state is `Failed`.

   The reconciled accounting of the last finalized run is kept (`lastRunAccounting`, with its start
   generation) for `fetch_run_accounting("last_run")` (ABI 31).

   The accounting line logged at the end of finalization is a WARN when
   `recording::needsOperatorAttention(completion)` (undeclared loss, failure or unknown) or when
   malformed frames exceed `kMalformedWarnFraction` (0.1 %) of the admitted frames, INFO
   otherwise (#549). Classification: `storeOverwritten`, `storeNotCommitted`, `processingFailed`
   and `sequenceGaps` are undeclared (`IncompleteLoss`); a booked `storeMalformed` frame is a
   declared loss (`IntentionallyPartial`). `finalizationOk` only says the file was written: a run can finalize cleanly
   and still be `IncompleteLoss`, which the UI shows from `completion` and `completion_reason`.

The worker also runs the periodic flush while Active: every 250 ms it
submits `flushBufferedFrames(hdf5)` when
`ProcessingService::needsFlush()` returns true (`status().flushing` is
true during the submission). `needsFlush()` fires on the frame-count
interval **or** a 50 % byte-budget watermark (issue #407 — the count-only
gate never opened when the byte budget saturated first). A 2-second
time-based backstop also flushes any non-empty buffer regardless of
thresholds, so a slow trickle of large frames never sits unwritten.
- `reportUnresolvedFault(code, message)` / `clearUnresolvedFault()`: a save
  or provenance failure from the last run blocks the next Start
  (`lifecycle.fault` gate) until the operator acknowledges it.

## Gates

| id | Fail / Unavailable when | Warn when |
|---|---|---|
| `camera.session` | capture not `Running` + `cameraReady` (reason includes the session's `lastFailure`) | — |
| `camera.source` | requested hardware fell back (`CameraSourceInfo.fallback`) or source unknown | explicit mock camera |
| `camera.deliveryMode` | not confirmed by a running backend | `latestFrame` (intentional drops) |
| `camera.geometry` | no frame received yet | — |
| `processing.roi` | — | no ROI (full frame) |
| `processing.core` | pinned core not active | — |
| `method.revision` (#398 M2) | applied config.json is a cached central revision that is revoked or not published/superseded (`NotRequired` for a local method or no config) | central revision not validated on this instrument/context, a failed local validation, or unknown instrument |
| `calibration.pixelToMicron` | factor not positive | — |
| `processing.background` | — | no background image |
| `trigger.output` | sorting enabled but TriggerService not bound to the running session (`NotRequired` when sorting is off) | — |
| `storage.output` | no path (`Unavailable`), unwritable parent, path is a directory, < 64 MiB free | — |
| `storage.hdf5` / `lifecycle.recording` / `lifecycle.experiment` | file already open / raw recording active / coordinator not Idle | — |
| `lifecycle.fault` | unresolved fault reported | — |
| `telemetry.transportLoss` | no active session | backend cannot / has not reported transport loss |

## Method provenance (#398 M2)

`candidateLocked` resolves `RunConfigurationSnapshot::method` with the pure
`resolveMethodProvenance()` (`include/backend/app/MethodProvenance.h`):
`canonicalConfigSha256(getLastConfigJson())` is matched against the
`configSha256` of every revision in the registry worker's **value snapshot**
(no network, no SQLite on the caller's thread), then the newest matching local
validation for this instrument UUID + `methodContextHash(backend.methodContext())`
+ exact content hash. Several revisions sharing a config: usable state first,
then validated here, then published over superseded, then newest
(`matching_revisions` records the count). The result is memoized on (raw
config sha, registry generation, context hash, instrument name) because
readiness is polled. `methodInvalidationKey()` is an invalidation input, so
recording a validation, a revocation reaching the cache, or a core/camera
change bumps the readiness generation and a stale preflight is refused.
Policy (operator decisions): unvalidated → Warn (Start allowed); validated
here → Pass; revoked → Fail (existing runs stay reviewable).

## RunConfigurationSnapshot (schema v2)

Frozen at Start and never mutated: readiness/start/capture generations,
start times, `CameraSourceInfo` (requested vs effective, simulated,
fallback + reason), delivery modes, `TimestampDescriptor` text, ROI, frame
geometry, processing-core identity + pin state, processing config version +
canonical sha, raw `config.json` sha, profile id, pixel-to-micron factor,
background presence/generation/sha, trigger requirement/binding, output
path, realtime mode, application version/build/OS, and (v2, #398 M2) the
`method` block: source central/local/none, revision/method/project IDs,
display name, author, exact content hash, revision number, metadata version,
central state, matching revisions, instrument UUID + name, context hash,
validation (passed/failed/none/notApplicable), validator, time, evidence
test-run SHA-256, registry origin + session. `runSnapshotToJson()` /
`readinessToJson()` produce the stable-key JSON stored on `/run_provenance`
(see [[../data-model/HDF5-Storage]]).

## Threading

`mutex_` serializes evaluate/start/requestStop/fault calls and the worker's
state changes; the worker releases it for every I/O step (flush,
metadata, close) so `status()` stays cheap during finalization. The status
callback is invoked with the mutex released. Evaluation only reads
service snapshots (`lifecycleSnapshot()`, `telemetrySnapshot()`,
`getProcessingConfig()`, …) and never takes a service lock while holding one
of its own for long. Safe to call from a UI timer. `start()` holds the mutex
across the HDF5 open + provenance write, which is why a second caller gets
`Busy` instead of racing.

## Gotchas

- The preflight must be evaluated with the **same** `outputPath` and
  `profileId` the Start presents — both are invalidation inputs.
- `camera.source` fails on a *fallback* (hardware requested, mock built) and
  only warns on an *explicit* mock selection; [[AppBackend]] records the
  distinction (`cameraSourceInfo()`), never silently.
- `finish()` does **not** finalize anything any more; call `requestStop()`
  and wait for `status().terminal`. Clients that call `Hdf5Service::closeFile()`
  themselves race the worker.
- A `requestStop()` right after `start()` returned is accepted; the worker
  wakes immediately (condition variable), so the run may finalize with zero
  admitted frames and still be `Complete`.

## MindVision overview gate

Readiness includes a failing `camera.mode` gate while MindVision Overview is
selected. Experiments require the Experiment acquisition mode, preventing a
full-sensor preview session from being recorded as an experimental ROI session.

### Shell-independent processing ownership (2026-09-23)

Start enables and starts the shared realtime consumer after persistence setup.
It no longer relies on a Qt tab activation side effect: otherwise a Tauri run
could finalize an empty file as complete. The facade's settings commands own
explicit realtime enable/start and disable/stop and serialize with the idle
configuration gate. The headless lifecycle regression asserts an enabled,
running consumer immediately after Start; native webview acceptance also checks
nonzero persisted accounting.

Readiness also blocks while bounded background calibration is running, preventing
a later publication from replacing a background after experiment configuration
is frozen. Cancellation/completion restores this gate.

Background set/clear and calibration apply share `withIdleConfiguration` with ROI
settings. Asynchronous calibration publication uses the nonblocking overload to
avoid waiting on a configuration transaction that might join its worker (#542).

### Raw recording admission (#451)

`AppBackend::startFrameRecording` uses `withIdleConfiguration` through writer
acquisition, so experiment Start and raw recording cannot both pass preflight.
Starting/Active/Stopping (and Failed until fault acknowledgement) refuse recording; an already
open HDF5 file is preserved. Raw recording remains busy until Stop joins its
worker, including save-failure cleanup. See [[AppBackend]].

### Nested idle configuration (#582)

An idle transaction can invoke guarded service setters on the same thread without
relocking the coordinator mutex. A scoped thread-local owner reuses only that
enclosing idle authorization and restores it even on exceptions. Other threads
still serialize against Start. This supports both facade transactions and Qt
watched-document applies without bypassing the backend ROI/background gates.

### Save failure accounting and recovery (#589)

Finalization marks fatal save errors and unsuccessful flushes as fatal accounting
before persisting `/experiment_info` and caching the last run. The persisted
completion is Failed, with the save error reason and failed persistence counts,
even when metadata can still be written. `acknowledgeFault()` returns the
coordinator to Idle only after finalization and for the matching run/fault
revision; it preserves the failed saved-run outcome. Qt readiness and banner
actions both use this contract.
