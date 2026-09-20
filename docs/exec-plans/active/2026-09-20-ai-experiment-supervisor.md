# AI Experiment Supervisor: JEV shadow-mode feasibility (#422)

Status: active

Issue: https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/422
Baseline: `7b3243c` on `develop` (after #421 device discovery).
ADR: [0006 — shadow-mode backend service behind a provider seam](../../decisions/0006-ai-experiment-supervisor.md)
Vault: `knowledge_map/services/SupervisorService.md`,
`knowledge_map/task/2026-09-20-ai-experiment-supervisor.md`
How-to: [`docs/howto/supervisor-evaluation.md`](../../howto/supervisor-evaluation.md)

## Goal

MIB Studio can freeze a versioned, replayable experiment snapshot from the
authoritative backend, ask interchangeable decision providers narrow typed
questions behind a deterministic policy that the model can never override,
audit every decision with its distributions/confidence/latency/errors, run
that beside a real experiment in shadow mode with zero actuation authority,
and measure agreement, dangerous disagreements, calibration, latency,
availability and cost with a repeatable offline harness — so a separate
decision about bounded control can be made on evidence.

## Acceptance criteria (issue numbering)

- [x] 1. Versioned, replayable `ExperimentSnapshot` built from the backend
      (`ExperimentSnapshotBuilder`; canonical JSON + SHA-256; unknown ≠ 0).
- [x] 2. Providers interchangeable behind `DecisionProvider`
      (`RuleProvider`, `JevProvider`, scripted test providers).
- [x] 3. Hard policy evaluated separately, cannot be overridden
      (`evaluateSafetyPolicy` before the provider; `decideOnce` precedence
      test).
- [x] 4. CI has deterministic mock scenarios, no JEV dependency
      (`tests/supervisor/*.json`, `backend.supervisor_harness`,
      `backend.supervisor_eval_rule`).
- [x] 5. Opt-in JEV provider produces schema-validated `DecisionResult`s
      (fake-transport tests; live via `supervisor_eval_run --provider jev`).
- [x] 6. Provider calls cannot block or alter acquisition/processing/
      recording/stop/finalization (`integration.e2e_supervisor_shadow`:
      hung provider beside a live mock run; stop/finalize/shutdown bounded).
- [x] 7. Shadow mode runs during an experiment with zero hardware actuation
      authority (service holds no hardware reference; every record
      `executed=false`).
- [x] 8. Decisions, distributions, confidence, latency, errors and operator
      actions auditable (`DecisionRecord` JSON-lines sidecar).
- [x] 9. Repeatable evaluation report (agreement, unsafe disagreements,
      calibration, latency, availability, cost) — `runEvaluation` +
      `supervisor_eval_run`.
- [x] 10. JEV failure/unavailability leaves ordinary operation unaffected
      (NotConfigured / timeout / cancel paths in e2e + service tests).
- [x] 11. No autonomous hardware control introduced.
- [x] 12. Documentation for the offline evaluation and the opt-in live test
      (`docs/howto/supervisor-evaluation.md`).

## Phases

- [x] **A — local foundation**: metric inventory (builder comments name the
      owner of every field), `ExperimentSnapshot` v1, `DecisionProvider` /
      `DecisionResult`, `SafetyPolicy`, serialization/replay, `DecisionRecord`.
- [x] **B — deterministic evaluation**: `RuleProvider`, 18 labelled fixtures
      (`tools/supervisor_gen_fixtures.py`), `EvaluationHarness`, CI lane.
- [x] **C — JEV integration**: `JevProvider` (pinned model, typed
      validation, distributions/confidence/latency/cost, timeout/retry,
      credential boundary), opt-in `supervisor_eval_run --provider jev`
      (libcurl transport in the tool only; ADR 0002 keeps the backend
      HTTP-free).
- [~] **D — shadow mode**: non-blocking `SupervisorService` scheduler,
      enable/disable configuration (env + API), sidecar persistence with
      operator-action association, `AppBackend` ownership/shutdown order,
      e2e verification with a hung provider. **Not done:** the minimal
      supervisor UI panel (issue §9) and bridge/Tauri exposure of
      `SupervisorStatus` — follow-up PR (see Open items).
- [ ] **E — feasibility report**: run the labelled dataset against live JEV
      (needs credentials + endpoint), run selected real experiments in
      shadow mode, produce calibration/disagreement/latency/availability/
      cost results, document failure cases, open a separate issue if
      bounded control is justified.

## Decision log

- 2026-09-20: module lives in `mib_backend` (not `mib_processing`): it reads
  app-level services, and the processing ABI must not grow a supervisor.
- 2026-09-20: provenance is a versioned JSON-lines sidecar
  (`*.supervisor.jsonl`) next to the data dir, not an HDF5 change; the HDF5
  schema version is not bumped by this issue.
- 2026-09-20: `frames_processed` in the snapshot is "frames delivered to the
  FrameStore" (capture telemetry), separate from object counts, because the
  realtime loop does not expose its own consumed-frame counter publicly.
- 2026-09-20: exposure and requested FPS stay `unknown` in live snapshots
  (camera-profile values, not live backend metrics); fixtures carry them so
  the contract is ready when the backend exposes them.
- 2026-09-20: the rule provider's "distributions" are synthetic margins
  (documented as such) so calibration code paths are exercised
  deterministically; they are not probabilities of correctness.
- 2026-09-20: `supervisor_eval_run` is a standalone test executable that
  links libcurl when found; the backend library gains no HTTP dependency.

## Open items / follow-ups

- Qt + React/Tauri supervisor panel (issue §9) rendering `SupervisorStatus`
  (mode, assessment, proposed action, selected probability, provider
  confidence, latency, provider error state, "recommendation only" banner)
  and a bridge-contract addition for it (ADR 0004 append-only).
- Expose exposure / requested FPS as live backend metrics so the snapshot
  can carry them (MindVision config values are file-side today).
- Live JEV run of the fixture dataset + 100–300 recorded decision points
  (phase E) once an endpoint and credential are available.
- HDF5 provenance group for supervisor records when the schema next bumps.
