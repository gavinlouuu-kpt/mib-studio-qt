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
- `finish()` no longer changes state: it returns the active (or most recently
  finalized) run snapshot for callers that log the identity.
- `status()` / `setStatusCallback(cb)`: `ExperimentStatus` (state,
  generations, output path, start/end wall-clock, live buffered counts,
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
failed finalization (fault latched); `Idle` after a clean one.

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
   remainder counts, processing config, ROI, background, core identity);
   `writeRunAccounting(experimentAccountingSnapshot())`;
   `writeAcquisitionProvenance(...)`; `writeConfigJson(getLastConfigJson())`.
6. `closeFile()`.
7. Restore the realtime mode if Start switched it.
8. Terminal status: `terminal=true`, `completion` from the reconciled
   accounting (`Failed` for a fatal save error), `finalizationOk=false` if
   2/4/5/6 failed, in which case the unresolved fault
   `experiment.flushFailed` / `experiment.provenanceFailed` /
   `experiment.saveFailed` is latched and the state is `Failed`.

The worker also runs the periodic flush while Active: every 250 ms it
submits `flushBufferedFrames(hdf5)` when the buffered count reaches
`ProcessingService::getFlushInterval()` (`status().flushing` is true during
the submission).
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
| `calibration.pixelToMicron` | factor not positive | — |
| `processing.background` | — | no background image |
| `trigger.output` | sorting enabled but TriggerService not bound to the running session (`NotRequired` when sorting is off) | — |
| `storage.output` | no path (`Unavailable`), unwritable parent, path is a directory, < 64 MiB free | — |
| `storage.hdf5` / `lifecycle.recording` / `lifecycle.experiment` | file already open / raw recording active / coordinator not Idle | — |
| `lifecycle.fault` | unresolved fault reported | — |
| `telemetry.transportLoss` | no active session | backend cannot / has not reported transport loss |

## RunConfigurationSnapshot (schema v1)

Frozen at Start and never mutated: readiness/start/capture generations,
start times, `CameraSourceInfo` (requested vs effective, simulated,
fallback + reason), delivery modes, `TimestampDescriptor` text, ROI, frame
geometry, processing-core identity + pin state, processing config version +
canonical sha, raw `config.json` sha, profile id, pixel-to-micron factor,
background presence/generation/sha, trigger requirement/binding, output
path, realtime mode, application version/build/OS. `runSnapshotToJson()` /
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
