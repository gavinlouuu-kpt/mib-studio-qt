# ADR 0006 — The AI Experiment Supervisor is a shadow-mode backend service behind a provider seam

- Status: accepted
- Date: 2026-09-20
- Issue: #422
- Plan: [2026-09-20-ai-experiment-supervisor](../exec-plans/active/2026-09-20-ai-experiment-supervisor.md)

## Context

Issue #422 asks whether JEV can make useful, bounded experiment decisions
from MIB Studio's structured runtime metrics, measured reproducibly, before
anyone considers giving a model any control. The authoritative C++ backend
owns acquisition, readiness, configuration validation, processing,
recording/finalization, hardware and safety; Qt and React/Tauri are
presentation adapters (ADR 0001). Nothing in this feasibility work may weaken
that hierarchy, add a second state machine, put a network call on an
acquisition thread, or let free-form model text become a command.

## Decision

1. **One Qt-free module, `backend::supervisor`, inside `mib_backend`**
   (`include/backend/supervisor/`, `src/backend/supervisor/`). Value types,
   C++17, no vendor handles, no camera/trigger/recording references. Its
   only inputs are a snapshot-producing function and a `DecisionProvider`;
   its only outputs are `DecisionRecord`s. There is structurally no
   actuation path.
2. **Frozen, versioned snapshot.** `ExperimentSnapshot` (schema 1) copies
   metrics the backend already produces (CaptureService telemetry and
   lifecycle, ProcessingService counters and monitoring window,
   TriggerService counters, ExperimentCoordinator status, frozen
   `RunConfigurationSnapshot`). Every metric the backend cannot observe is
   `Metric{known=false}` (serialized `null`), never a measured zero. The
   canonical JSON is byte-stable and hashed (SHA-256) so a decision can be
   replayed offline. Exposure/requested FPS are not exposed as live metrics
   by the backend today and stay unknown rather than guessed.
3. **Closed decision contract (version 1).** Five typed questions
   (`run_quality`, `primary_problem`, `next_action`, `adjustment_target`,
   `adjustment_direction`) with the vocabularies from the issue. Parsers are
   strict: an unknown token is rejected, never defaulted. Providers return
   full distributions, confidence, latency, attempts, cost and a typed
   `ProviderError`; on any error the answers are the fail-closed
   `HUMAN_REVIEW`.
4. **Deterministic policy precedes the model and cannot be overridden.**
   `evaluateSafetyPolicy` runs in ordinary code before any provider call:
   unresolved fault / failed run, storage failure, camera unavailable while
   active, hard transport frame-loss fraction, target count reached,
   experiment not active, insufficient observations. When it fires, the
   provider is not consulted and the record says `decided_by = "policy"`.
   `evaluateEligibility` re-checks every returned answer against the
   configured limits (allowed targets, coherent ADJUST fields, optional
   selected-probability threshold); in shadow mode the flag is recorded and
   never acted on.
5. **Provider seam, not a JEV controller.** `DecisionProvider` is the only
   abstraction the service knows. `RuleProvider` (deterministic thresholds)
   is the CI baseline and the default at boot. `JevProvider` is an adapter
   over an injected `HttpPostFn` (the ADR 0002 pattern: the backend has no
   HTTP client; the shell or the evaluation tool supplies the transport).
   It pins the model, validates every response against the local contract,
   maps HTTP/transport failures to typed errors, retries only timeouts /
   429 / 5xx once, reads the credential from an environment variable at
   call time and never copies it into results, records or logs.
6. **Provenance is a versioned sidecar first.** Every evaluation appends one
   `DecisionRecord` (schema 1: timestamp, snapshot hash + inline snapshot,
   policy outcome, provider result with distributions/confidence/latency/
   error, recommendation, eligibility, `executed=false`, later operator
   action / outcome label) to `<data>/supervisor/*.supervisor.jsonl` with a
   header line. The stable HDF5 schema is untouched; the eventual
   integration is a `/provenance/supervisor` group carrying the same records
   once the schema version bumps (tracked in the plan, not done here).
7. **Shadow mode is off by default and off the critical path.**
   `SupervisorService` runs one worker at a bounded cadence (≥ 500 ms,
   default 5 s), only while an experiment is active, and consults the
   provider on that worker. `configure/status/requestStop/recordOperatorAction`
   never wait on a provider; `shutdown()` cancels an in-flight call and
   joins. `AppBackend::shutdown()` stops the supervisor before any service
   it observes. Enablement: `MIB_SUPERVISOR_MODE=shadow`
   (`MIB_SUPERVISOR_PROVIDER=rule|jev`, `MIB_SUPERVISOR_INTERVAL_MS`,
   `MIB_SUPERVISOR_OBJECTIVE`, `MIB_SUPERVISOR_TARGET_VALID`).
8. **Evaluation is a harness plus reviewable fixtures.** `runEvaluation`
   drives every labelled case through the same `decideOnce` path as live
   shadow mode and reports per-question agreement, confusion matrices,
   false STOP / false CONTINUE, unsafe disagreements, HUMAN_REVIEW rate,
   adjustment target/direction agreement, latency p50/p95, provider failure
   rate, cost, and `P(correct | selected probability ≥ t)` /
   `P(correct | confidence ≥ t)` for configurable thresholds. Fixtures live
   in `tests/supervisor/*.json` (18 scenarios covering the 15 from the
   issue), generated by `tools/supervisor_gen_fixtures.py`, versioned
   (`dataset_version`), and the label is rejected if it appears inside the
   snapshot. CI runs the rule provider over them; live JEV is opt-in through
   `supervisor_eval_run --provider jev`.

## Consequences

- Adding a provider (another model, a deterministic optimizer) means
  implementing `DecisionProvider`; the policy, records, harness and shadow
  scheduler are inherited unchanged.
- Confidence is recorded but never used as a safety signal; threshold
  selection is an empirical output of the harness.
- The UI panel (issue §9), bridge/Tauri exposure and any actuation path are
  explicitly out of this ADR. Bounded single-parameter control (issue §11)
  requires a follow-up issue and its own review gate.
- The sanitizer lane covers the service (`backend;supervisor;concurrency`
  labels); the shadow worker must stay free of naked joins.
