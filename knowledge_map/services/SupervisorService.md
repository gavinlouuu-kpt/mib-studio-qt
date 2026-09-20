# SupervisorService

> AI Experiment Supervisor in **shadow mode** (issue #422, ADR 0006). Freezes
> a versioned `ExperimentSnapshot` from the authoritative backend at a bounded
> cadence, runs the deterministic `SafetyPolicy`, asks a `DecisionProvider`
> (rule baseline or JEV) five closed-vocabulary questions, and appends an
> auditable `DecisionRecord`. It recommends; it never actuates. There is no
> code path from a recommendation to the camera, trigger, processing,
> recording or any hardware.

**Source:** `src/backend/supervisor/{DecisionContract,ExperimentSnapshot,SafetyPolicy,RuleProvider,DecisionRecord,SupervisorService,EvaluationHarness,JevProvider,ExperimentSnapshotBuilder}.cpp`,
`include/backend/supervisor/*.h`
**Tests:** `tests/backend/supervisor_{contract,policy,harness,service,jev_provider}_test.cpp`,
`tests/integration/e2e_supervisor_shadow_test.cpp`,
`tests/tools/supervisor_eval_run.cpp` (CTest `backend.supervisor_*`,
`integration.e2e_supervisor_shadow`; sanitizer lane via the `backend` label)
**Fixtures:** `tests/supervisor/*.json` (18 labelled scenarios; generator
`tools/supervisor_gen_fixtures.py`)
**Related:** [[../architecture/AppBackend]] (owner, env wiring, shutdown
order), [[../architecture/ExperimentCoordinator]] (run state the snapshot
mirrors), [[CaptureService]] (telemetry), [[ProcessingService]] (counters,
monitoring window), [[TriggerService]] (counters),
[[../task/2026-09-20-ai-experiment-supervisor]],
`docs/decisions/0006-ai-experiment-supervisor.md`,
`docs/exec-plans/active/2026-09-20-ai-experiment-supervisor.md`,
`docs/howto/supervisor-evaluation.md`

## Responsibility

- **Snapshot** (`ExperimentSnapshot`, schema 1): identity/state
  (sequence, run id, experiment state, elapsed, objective, target count),
  configuration subset (camera source/simulated, delivery mode, ROI,
  detection threshold, area range, trigger enabled, core version, config
  hash — never raw hardware values), acquisition (capture lifecycle, per-
  metric telemetry), detection (counters, fps, window statistics: area,
  contrast = Q3−Q1, median/max brightness, rejection reasons), trigger
  counters, recording/fault state, prior recommendations. A metric the
  backend cannot observe is `Metric{known=false}` → JSON `null`, never 0.
  `snapshotToJson` is canonical (sorted keys) and `snapshotHash` is its
  SHA-256, so any record replays exactly. `ExperimentSnapshotBuilder`
  reads `AppBackend` services read-only; exposure / requested FPS stay
  unknown (not live backend metrics yet).
- **Contract** (`DecisionContract`, version 1): `RunQuality`,
  `PrimaryProblem`, `NextAction`, `AdjustmentTarget`,
  `AdjustmentDirection` with strict parsers (unknown token → `nullopt`,
  never a default). `DecisionResult` carries answers, per-question
  `Distributions`, `confidence`, `rationale` (display only), latency,
  attempts, cost and a typed `ProviderError`; `ok()` false ⇒ answers are
  the fail-closed `HUMAN_REVIEW`.
- **Policy** (`SafetyPolicy`): rules in fixed order —
  `policy.unresolved_fault`, `policy.storage_failure`,
  `policy.camera_unavailable`, `policy.frame_loss` (transport loss fraction
  ≥ `hardFrameLossFraction`, default 0.20), `policy.target_reached`,
  `policy.not_active`, `policy.insufficient_data` (< `minFramesForModelDecision`
  frames or < `minElapsedSecondsForModel`). First hit wins; the provider is
  not consulted. `evaluateEligibility` checks returned answers (allowed
  targets, coherent ADJUST target/direction, optional selected-probability
  threshold); shadow mode only records the flag.
- **`decideOnce(snapshot, policy, provider, mode)`** is the single decision
  path for the worker, `evaluateNow`, and the harness: policy → provider
  (exceptions contained) → eligibility → `DecisionRecord`
  (`decided_by = policy | provider | fail_closed`, `executed = false`).
- **Service** (`SupervisorService`): `setProvider` (refused while
  running), `setSnapshotSource`, `configure(SupervisorConfig)` (mode Off
  by default, `intervalMs ≥ minIntervalMs` 500, `onlyWhileExperimentActive`,
  `logPath`, `runId`, objective/target, `DecisionPolicy`), `start` /
  `requestStop` (non-blocking, calls `provider->cancel()`) / `shutdown`
  (joins), `evaluateNow`, `recordOperatorAction` (association only, also
  appended to the sidecar), `status()` (`SupervisorStatus`: mode, running,
  inFlight, provider identity, evaluation/policy/provider/failure counters,
  `lastError`, `lastRecord`, `recommendationOnly = true`), observers.
  One worker thread; no naked joins; control-plane calls never wait on a
  provider.
- **Sidecar** (`DecisionLog`): append-only JSON-lines with a header
  (`supervisor_log`, schema/contract versions, run id, provider, `executed:false`)
  at `<data>/supervisor/shadow-<epoch>.supervisor.jsonl`; `readAll` for
  offline replay. HDF5 untouched.
- **Providers**: `RuleProvider` (`rule/1`, deterministic thresholds in
  `RuleThresholds`, synthetic margin "distributions" — not probabilities of
  correctness) and `JevProvider` (`jev-adapter/1`) over an injected
  `HttpPostFn` (`HttpPostRequest{url, headers, body, timeoutMs}` →
  `HttpPostResult{ok, status, body, error, timedOut}`): pinned model,
  request with contract version + closed vocabularies + canonical snapshot,
  strict response validation, error mapping (401/403 → Authentication, 429
  → RateLimited, 5xx/transport → Transport, timeout → Timeout, JSON →
  MalformedResponse, contract → SchemaViolation, model → ModelMismatch),
  one bounded retry on timeout/429/5xx, cancellable backoff, credential
  from `apiKeyEnvVar` at call time (never in results/records/logs).
- **Harness** (`EvaluationHarness`): `LabelledCase` (case schema 1,
  `dataset_version`, `expect_policy`), `loadLabelledCases(dir)`,
  `runEvaluation` → `EvaluationReport` (per-question agreement + confusion,
  false STOP / false CONTINUE / unsafe, HUMAN_REVIEW rate, adjust target/
  direction agreement, latency p50/p95/max, provider failure rate, cost,
  calibration buckets for selected probability and confidence),
  `reportToJson` / `reportToText`. Tool: `supervisor_eval_run
  --fixtures <dir> --provider rule|jev [--out] [--text] [--calibration]
  [--fail-on-unsafe]` (libcurl transport only in the tool).

## Rule provider — decision order

frame loss ≥ hard limit → STOP_FAILURE · loss ≥ 2 % → HUMAN_REVIEW
(FRAME_LOSS; also the "good detection + loss" conflict) · trigger failure
fraction ≥ 10 % → ADJUST TRIGGER_TIMING INCREASE · pulses > eligible →
HUMAN_REVIEW (TRIGGER_FAILURE) · Q4 mean ≥ 250 → ADJUST EXPOSURE DECREASE
(OVEREXPOSURE) · Q2 mean ≤ 25 → ADJUST EXPOSURE INCREASE (UNDEREXPOSURE) ·
contrast < 20 → ADJUST EXPOSURE INCREASE (LOW_CONTRAST) · no objects after
10 s → HUMAN_REVIEW (INSUFFICIENT_DATA) · ≥ 400 obj/s and rejection ≥ 75 %
→ ADJUST DETECTION_THRESHOLD INCREASE (EXCESS_FALSE_DETECTIONS) · mean
area < ½ min area and rejection ≥ 75 % → ADJUST MIN_AREA INCREASE ·
rejection ≥ 75 % → ADJUST DETECTION_THRESHOLD DECREASE (EXCESS_REJECTIONS)
· ≥ 150 obj/s → HUMAN_REVIEW (OTHER) · else CONTINUE (GOOD when rejection
is known, else MARGINAL/INSUFFICIENT_DATA). The fixtures under
`tests/supervisor/` are the reviewable spec of this table.

## Gotchas

- `frames_processed` in the snapshot is "frames delivered to the
  FrameStore" (capture telemetry), not an object count; object totals are
  `valid_objects` / `invalid_objects` (flushed + buffered).
- Window statistics (area/contrast/brightness, rejection reasons) exist
  only while `ProcessingService::isMonitoringActive()`; otherwise they are
  unknown and the rule provider falls back to counter-based rules.
- Trigger `eligible_objects` = issued + suppressed + stale requests;
  `triggers_issued` = issued − post-dequeue pulse losses.
- `evaluateNow` works while the mode is Off (harness/operator use) and
  records with mode `shadow`; the worker never runs while Off.
- A provider that ignores `cancel()` and its timeout can delay
  `shutdown()` by its remaining timeout; `JevProvider` and the test fakes
  honour both. `AppBackend::shutdown()` stops the supervisor first.
- Replacing the provider while the worker runs is refused; stop first.
- Never put a label (`expected`) inside a fixture snapshot; the loader
  rejects it so the model input cannot leak the answer.

**Up**: [[_MOC|Services MOC]] · [[../README|Vault home]]
