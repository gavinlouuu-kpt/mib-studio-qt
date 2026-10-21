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
- **Output path (#762).** `acceptOutputPath(requested)` is the one place a
  start path is accepted: `.h5` is appended when the path has no HDF5
  extension, then `AppBackend::resolveClientPath` (`app::resolveClientPath`
  under `AppBackend::storageRoot()`, the data directory made absolute with
  symlinks resolved; [[AppBackend]]) puts a relative path under the storage
  root. An absolute path is unchanged (the native dialogs); with no root
  configured (an uninitialized backend) a relative path stays relative, as
  before. A path whose `weakly_canonical` form leaves the root (`..`, a
  symlink as the file or a parent, compared by component) or that goes
  through a dangling symlink is refused. `evaluateReadiness` and `start` both
  call it, so the probes run in the resolved directory and the candidate, the
  snapshot `output_path`, the run, the status and the start reply
  (`experiment started: <path>`) carry the resolved absolute path. Readiness
  for a refused path fails `storage.output` with the reason and probes
  nothing.
- `start(ExperimentStartRequest{outputPath, readinessGeneration, profileId,
  acknowledgeLatestFrameDrops, acknowledgeDiscardRing, declared})` is the serialized Start transaction:
  0. `declared` (the client's identity and planned annotation, bridge ABI 39,
     `recording::StartRequestIdentity`) is checked with
     `recording::validateStartRequest` before anything else: a refused one is
     `InvalidRequest`, whatever the state, and opens nothing. Then the output
     path (`acceptOutputPath`): empty, or leaving the storage root, is
     `InvalidRequest` too ("no output path given: choose where the run file
     goes" / "the output path … leaves the data directory …"), whatever the
     state, and opens nothing.
  1. A run in progress gets `AlreadyActive` first, from the atomic `state_`,
     before any lock (a status poll or the worker holding the mutex used to
     turn it into `Busy`, #595). Then `try_lock` — a concurrent transaction
     gets `Busy`.
  2. Re-evaluate now; `readinessGeneration` mismatch → `StaleReadiness`
     (reconnect, config/background/core/ROI/output change, new fault since
     the presented preflight). A declared `calibration_record_id` that is not
     the applied calibration record in effect at this evaluation (#716 A4a,
     ABI 42; also one declared while none is in effect) → `InvalidRequest`
     ("… (mismatch)"), nothing opened. Not ready → `NotReady`. LatestFrame
     without acknowledgement → `NotReady`.
  3. Freeze the `RunConfigurationSnapshot` from the evaluated candidate
     (start generation, host + wall-clock start time, output path), then the
     snapshot v3 facts (#712): a fresh `run_id` (UUID v4) and
     `captureRunFacts` (E-modulus LUT identity, pumps, PZ7035 LED strobe
     registers, Z stage, piezo; see "Snapshot v3 facts" below). On a PL run the
     PL run id (`RUN_ID`: the SSD run id, otherwise the start time in ms) is
     chosen here too, after the SSD prepare and before the snapshot is written,
     so the snapshot names the id the provider is armed with. The declared
     identity is copied into `run.v3.identity` as given, except
     `calibration_record_id`, which is the backend's applied record in effect
     (`ExperimentReadinessSnapshot::calibrationRecordId`, null when none; #716
     A4a). A run of a calibration method remembers its method spec and the
     identity at Start (`activeCalibration_`) for the derivation at close.
  4. Open the HDF5 file + `initializeDatasets()` (`StorageFailed` rolls back
     to Idle), then persist the snapshot **first** via
     `Hdf5Service::writeRunSnapshotJson` (`ProvenanceFailed` closes and
     removes the file). A run without its frozen snapshot is never Running.
     A declared annotation is written right after it, once, into
     `/experiment_annotation` (`writeExperimentAnnotationJson`); a failure is
     `ProvenanceFailed` with the same rollback.
  5. `setExperimentAccountingContext(captureGeneration, latestFrame)` +
     `ProcessingService::startExperiment()`; start the worker thread (first
     run only); publish `Active`. Before step 2 a multi-image series run
     switches the realtime mode `AsyncBatch -> Inline` (so the frozen
     snapshot records the mode actually used); the finalization restores it.
     Right before `startExperiment()` it calls
     `ProcessingService::setMonitoringRunGeneration(startGeneration)`, so the
     monitoring ring tags the run's rows (bridge ABI 41; reset to 0 if the
     provider start then fails), and at Active it calls
     `MonitoringCellStore::beginRun(startGeneration)`, which drops the live
     cell strip and every pin ([[../services/MonitoringCellStore]]).
- `requestStop(cancelled)` returns `ExperimentStopOutcome::Accepted` and
  hands the finalization to the worker; `Busy` while Starting/Stopping or a
  stop is already queued; `NotActive` when there is no run. Completion is
  observed through `status()` / the status callback (`terminal == true`).
- `requestStop(cancelled, expectedStartGeneration, refusal)` (#741, bridge ABI
  40, `experiment_stop`'s `expectedStartGeneration`) stops only the named run.
  The check runs first, under `mutex_`, in the critical section that sets
  `stopRequested_`: never check-then-act. The run *held* is `activeRun_`, set
  from the Active entry until the end of finalization (Active, Stopping, or
  Failed while a fatal finalization runs); a Starting run is never held (its
  generation is published only once it is Active, so no client can name it).
  Nothing held, or a held run with another `startGeneration`:
  `RunMismatch` (3), `refusal` gets `stop refused: expected run <E>, but run
  <N> is active | is stopping | failed and is finalizing | a new run is
  starting | no run is active; nothing was stopped`, nothing changes. A match
  falls through to the one-argument outcome. A stale generation can never stop
  a newer run: an accepted stop is consumed by the finalization of the run it
  named (the worker finalizes the run it holds), `start()` is refused until
  that finalization ends and clears any stop flag at the Active entry. Without
  a generation (every C++ caller, the Qt window, the server's client-loss stop)
  nothing changed. The facade returns the refusal as the message and the
  outcome as `experimentStopOutcome`, which the bridge carries as `stop_outcome`
  (the start outcome likewise as `start_outcome`). Guard:
  `e2e.experiment_stop_generation` (semantics, every start outcome through the
  facade, a stress test of stale stops racing start/stop cycles).
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
3. `endExperiment()`, then `setMonitoringRunGeneration(0)` (later monitoring
   rows are "outside a run"); `resetRealtimeMetrics()`.
4. Remainder that arrived between 2 and 3 goes through `flushBufferedFrames`
   + `finishFlush` again so `persistenceCommitted` credits it (a direct
   `appendFrames` left a clean run labelled IntentionallyPartial; bench,
   2026-09-08).
5. `Hdf5Service::flush()`; `writeExperimentInfo(...)` (start/end wall-clock,
   run-wide successful valid/invalid writes, processing config, ROI, background, core identity);
   for a host-science run, `writeObjectCoordinates(...)` (#756 review 2): `roi_local` when the
   frozen `realtimeMode` is `inline` and the ROI has an area (the inline path validates the ROI's
   mask: ROI-local boxes next to full-frame images and masks), `image` otherwise (async batch,
   whole image); a PL-science run's rows come from the PL and nothing is written;
   `writeRunAccounting(experimentAccountingSnapshot())`;
   `writeAcquisitionProvenance(...)`; `writeConfigJson(getLastConfigJson())`;
   then, best effort, `writeKdeLiveJson(...)` with the last provisional KDE
   core record [[../services/MonitoringDensityService]] handed over through
   `setLiveKdeCoreRecord` (from its backend worker, no shell involved)
   (accepted only while `Active`, cleared at Start; a failed write logs a
   warning and never changes the run outcome). The stop record (#712)
   `/run_provenance @run_stop_json` names the path that ended the run:
   `stop_requested` (every `requestStop(false)` caller, the UI's Stop and the
   bridge server's client-loss stop alike), `cancel_requested`,
   `fatal_save_error` (with the message) or `shutdown`. Best effort like the
   wall-clock provenance: a failed write logs an error, the run outcome is
   unchanged, and the file reads its stop reason as unknown.
6. `closeFile()`.
6b. A run of a calibration method (#716 A4a): `deriveCalibrationRecord` reads
   the closed file in a reader of its own (`readCalibrationEvidence`: valid
   objects, metadata only), hashes it, derives the record
   (`deriveCalibration`, see [[../services/CalibrationRecords]]) and appends it
   to the store, `proposed` or `insufficient` (a failed finalization adds the
   failing `run_finalized` gate). It runs with the lock released while the state
   is still `Stopping`/`Failed`, so no review reader can open beside it (the
   facade opens review sources only inside the idle transaction; HDF5 is not
   thread-safe, #722); it never changes the run outcome.
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
| `calibration.record` (#716 A4a) | the applied method's `calibration_policy` is `required` and no applied record is in effect (none applied, the factor changed since the apply, or derived under another setup); `Unavailable` when the camera/ROI identity is unknown or the store cannot be read; a malformed method fails closed. `NotRequired` for policy `none` (legacy methods) and for a calibration method itself. An identity change first appends the applied record's `invalidated` version (hard, once). Fix names the real action: "run the calibration method", "apply proposed record <id> (calibration_apply)…", or "re-apply record <id>…" | policy `warn` with the same conditions; the applied record is older than `calibration_max_age_hours` (soft: also under `required`); its age cannot be determined (clock behind) |
| `processing.background` | — | no background image |
| `trigger.output` | sorting enabled but TriggerService not bound to the running session (`NotRequired` when sorting is off) | — |
| `rf.generator` | sorting enabled and the configured [[../services/RfGeneratorService]] link is down, or the SSG3021X is mis-armed (RF off, pulse mod off, trigger mode not external, source not internal, zero width — reason lists them); `NotRequired` when sorting is off or `rf_generator.enabled` is false or absent (bundled defaults included) | — |
| `storage.output` | no path (`Unavailable`), a path that leaves the storage root (#762, nothing probed), unwritable parent, path is a directory, < 64 MiB free | — |
| `storage.hdf5` / `lifecycle.recording` / `lifecycle.experiment` | file already open (the shared handle, or a review file in the facade's ReviewSession: `AppBackend::reviewSourceOpen()`, #512 S5; remediation `kCloseReviewFile` for a file open for review, "finish or close the current file first" while raw recording or a run holds it, F2 of #763) / raw recording active / coordinator not Idle | — |
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

Trigger records: start discards whatever [[../services/TriggerService]] had
buffered before the run (test pulses, a previous run's tail); each periodic
flush carries the drained records inside the batch (see
[[../services/ProcessingService]] `setTriggerEventSource`); stop drains once
more after `finishFlush()` and appends the tail single-threaded before the
metadata writes, so `/trigger_events` is complete for the run. The RF
generator readback the readiness evaluation verified (candidate
`rfGenerator*` fields, also in the stored run snapshot JSON under
`rf_generator`) is written at finalize as `rf_generator_*` attributes
(`Hdf5Service::writeRfGeneratorProvenance`); the instrument is not
re-queried at stop.

## RunConfigurationSnapshot (schema v3)

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

### Snapshot v3 facts (#712, ADR 0015 item 5)

`RunConfigurationSnapshot::v3` (`recording::RunProvenanceV3`,
`include/backend/recording/RunProvenanceRecord.h`) is filled by the Start
transaction only; a readiness candidate leaves it unknown, so readiness polls
do none of this. `captureRunFacts` reads what the services already hold and
does no device I/O except the PZ7035 strobe registers (memory-mapped):

- `run_id`: `profiles::generateUuidV4()`; the FCS `$MIB_RUN_ID`.
- `emodulus_lut`: `ProcessingService::eModulusLutIdentity()` (SHA-256 of the
  loaded bytes plus the catalog's labels, see [[../services/ProcessingService]]).
- `pumps`: `SyringePumpService::runFacts(id, 150 ms)` per slot: the settings
  the pump is known to hold (`AppliedSettings`; never the configuration's
  defaults: a pump connected but not yet configured records them as unknown)
  and the last complete status poll with its age. `runFacts` polls the pump
  lock with `try_lock()` until a 150 ms deadline; a slot busy with serial I/O past 150 ms is recorded with
  `connected: null` (unknown) instead of holding Start behind a 1 s serial
  timeout per register.
- `led`: `AppBackend::readInstrumentLed` (S[0], S[1], S[2], S[10]) when the
  instrument has PZ7035 control, else null. `stopped_run_at_start` is set when
  the Start leaves a stopped Run: the LED is switched on with the Run preset
  after the readback (`leaveStoppedRun`), so the readback shows it off.
- `stage`: `StageService::snapshot()` (identity, zero, last published status
  and its age).
- `piezo`: `AutofocusService` atomics only (connected, last voltage read or
  written, autofocus on); the device mutex is held across serial I/O, so no
  backend kind or fresh read is taken.
- `pl_run_id`: the id armed into the PL (null for a host run).
- `experiment_id`, `run_index`, `sample_ref`, `operator`: the request's
  `declared.identity` (ABI 39), null when not declared; the operator is
  recorded with `assurance: "selected"` (the client's selection, #393 phase 1,
  not authentication).
- `calibration_record_id` (since ABI 42, #716 A4a): the backend's applied
  calibration record in effect at the Start's readiness evaluation (identity
  matches, its factor is the current factor), null when none. A declared id is
  only a check (step 2): it never reaches the snapshot.

## Threading

`mutex_` serializes evaluate/start/requestStop/fault calls and the worker's
state changes; the worker releases it for every I/O step (flush,
metadata, close) so `status()` stays cheap during finalization. The status
callback is invoked with the mutex released. Evaluation only reads
service snapshots (`lifecycleSnapshot()`, `telemetrySnapshot()`,
`getProcessingConfig()`, …) and never takes a service lock while holding one
of its own for long. Safe to call from a UI timer. `start()` holds the mutex
across the HDF5 open + provenance write, which is why a second caller gets
`Busy` instead of racing. The expected-run check of `requestStop` (#741) is
taken under the same mutex as the stop it guards.

## Gotchas

- The preflight must be evaluated with the **same** `outputPath` and
  `profileId` the Start presents — both are invalidation inputs. The key
  holds the accepted path (extension appended, resolved under the storage
  root), so "run" and "run.h5" evaluate the same file and generation.
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

### A review file blocks Start (#512 S5, decision D1 (a))

Before #512 S5 a file loaded for review sat in `AppBackend::hdf5()`, so
`storage.hdf5` failed and Start was refused until Close File. The facade now
reads review files with its own `ReviewSession` reader
([[../services/ReviewSession]]), and keeps the refusal on purpose: HDF5 is
built without thread safety and `Hdf5Service` has no process-wide lock
(#722), so a review reader beside the 5 kHz recorder would race inside the
library. The facade sets `AppBackend::setReviewSourceOpen(bool)` from its
session's `isOpen()` inside `withIdleConfiguration` (load, close, a failed
load; cleared at shutdown), through a scope guard that also runs when an
exception leaves the transaction, and `ReviewSession::open()` is all or
nothing, so the flag never reads closed while a review reader is open once
the coordinator mutex is released. The gate, Start's writer-conflict check and raw recording's
writer check all read `hdf5().isFileOpen() || reviewSourceOpen()` under the
coordinator mutex, so a review file and a file in the shared handle give the
same texts. Since F2 of #763 they say how to clear it
(`kCloseReviewFile` in `ExperimentReadiness.h`): gate `storage.hdf5` Fail, "an
HDF5 file is already open" / "Close the file in Review (Review → Close
File)"; Start NotReady "an HDF5 file is already open: Close the file in Review
(Review → Close File) before starting an experiment"; raw recording "an HDF5
file is already open: Close the file in Review (Review → Close File) before
starting raw recording". While raw recording holds the shared handle, Start
says "raw recording is active: stop it before starting an experiment" and the
gate keeps "finish or close the current file first" (also during a run). Like
the shared handle, the flag is not an invalidation input (no generation
bump). Tests: `backend.experiment_readiness` section 5b,
`backend.facade_review_session`, `backend.facade_boundary`,
`e2e.client_output_path`. Revisit after #722.

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

## Recording safety re-port (#403)

`requestFlush()` wakes the worker on buffer pressure using the existing #407
`needsFlush()` policy and two-second backstop. Stop waits for the inline loop
to hand off its incomplete multi-image series before settlement and remainder
flush. A handoff timeout makes finalization fail. Status observers are best
effort: exceptions are contained while callback retirement and the lifecycle
lock are restored. Fatal accounting is reconciled before writing the file,
aligned with #589's failed-outcome persistence.

Readiness includes `storage.roundtrip`: a small HDF5 write/close/reopen and
pixel comparison. It verifies access and format, not sustained throughput.
The destination probes that write to the disk (this round trip and the
`.mib_write_probe_*` temp file of `storage.output`) run once per (output
path, readiness generation), not on every evaluation, so a UI polling
readiness does not churn the SSD (#713). A path change, or anything else
that bumps the generation, probes again. Only a pass is kept: a failed probe
runs again at the next evaluation, so a destination the operator fixes
(permissions, antivirus, a mount) passes without a path change, and a
failing destination blocks Start anyway. The metadata checks of
`storage.output` (parent a directory, path not a directory, 64 MiB free)
write nothing and run on every evaluation. Test:
`backend.readiness_probe_cache`. `storage.buffer` rejects a host
image/mask series larger than the byte budget and warns when byte pressure
will precede the count threshold. Geometry/format, budget and flush interval
changes invalidate preflight. Metadata-only provider recordings do not use
the host image payload gate.

### Save failure accounting and recovery (#589)

Finalization marks fatal save errors and unsuccessful flushes as fatal accounting
before persisting `/experiment_info` and caching the last run. The persisted
completion is Failed, with the save error reason and failed persistence counts,
even when metadata can still be written. `acknowledgeFault()` returns the
coordinator to Idle only after finalization and for the matching run/fault
revision; it preserves the failed saved-run outcome. Qt readiness and banner
actions both use this contract.

## Flush backpressure (#597)

A flush request is a wake-up, not a promise that a batch will be submitted.
ProcessingService retains frames in its bounded experiment buffer when the
three-slot writer queue is full, allowing subsequent requests to coalesce them.
The existing count/byte gates and periodic backstop remain active. Stop first
drains the queue; after `endExperiment()`, its existing remainder flush writes
any deferred frames and partial series through a fresh queue. No coordinator
or bridge contract change is needed.
