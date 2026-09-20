# AI Experiment Supervisor: JEV shadow-mode feasibility (#422)

Issue: https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/422 — the
issue body is the specification. Plan:
`docs/exec-plans/active/2026-09-20-ai-experiment-supervisor.md`. Decision:
`docs/decisions/0006-ai-experiment-supervisor.md`. Service note:
[[../services/SupervisorService]]. How-to:
`docs/howto/supervisor-evaluation.md`.

## What changed

- **Backend module `backend::supervisor`** (`include/backend/supervisor/`,
  `src/backend/supervisor/`, compiled into `mib_backend`):
  `DecisionContract` (enums, strict parsers, `DecisionResult`,
  `DecisionPolicy`), `ExperimentSnapshot` (schema 1, canonical JSON,
  SHA-256 hash, derived fractions), `DecisionProvider`, `SafetyPolicy`
  (+ eligibility), `RuleProvider`, `DecisionRecord` + `DecisionLog`
  (JSON-lines sidecar), `SupervisorService` (shadow worker, `decideOnce`),
  `EvaluationHarness`, `JevProvider` (HTTP seam), `ExperimentSnapshotBuilder`
  (over `AppBackend`).
- **AppBackend** owns the service (`supervisor()`), wires the snapshot
  source and provider from env, starts shadow mode only on
  `MIB_SUPERVISOR_MODE=shadow`, exposes `setSupervisorHttpPost`, and stops
  the supervisor first in `shutdown()`.
- **Fixtures** `tests/supervisor/*.json` (18 scenarios: normal, low
  contrast, under/overexposure, threshold too low/high, debris, no objects,
  high concentration, severe frame loss, camera stall, trigger
  inconsistency/failure, contradictory state, target complete,
  insufficient early data, storage failure, unresolved fault) from
  `tools/supervisor_gen_fixtures.py`, `dataset_version fixtures/2026-09-20.1`.
- **Tool** `supervisor_eval_run` (standalone; libcurl only here).

## Tests (all network-free)

| CTest | Category | Covers |
|---|---|---|
| `backend.supervisor_contract` | round-trip + fault-injection | enum strictness, snapshot/record canonical round trip, null-not-zero, bad schema/JSON, sidecar read/write faults |
| `backend.supervisor_policy` | invariant | every policy rule + precedence, model cannot override, fail-closed on every error kind, eligibility limits |
| `backend.supervisor_harness` | round-trip | fixtures load/validate, rule baseline 100 % agreement, known-wrong providers give known confusion / false STOP / false CONTINUE / calibration counts, failures counted |
| `backend.supervisor_service` | concurrency + stress + fault-injection | off by default, cadence, skip-inactive, sidecar, hung provider never blocks control plane, cancel on stop, timeout, throwing provider/source, bad log path, 30 start/stop cycles with concurrent readers (watchdog) |
| `backend.supervisor_jev_provider` | fault-injection | request contents (pinned model, vocab, snapshot, credential header only), every schema rejection, 401/429/5xx/timeout/404 retry rules, not-configured without HTTP, credential never in JSON |
| `integration.e2e_supervisor_shadow` | pipeline e2e + concurrency | builder over a real `AppBackend` (idle → unknown; mock camera → live), real experiment start, hung provider beside the run: frames keep flowing, stop/finalize/shutdown bounded, zero actuation; env boot wiring (rule / unconfigured JEV fails closed) |
| `backend.supervisor_eval_rule` | — | the CI evaluation report over the fixtures with `--fail-on-unsafe` |

## Open (phase D UI / phase E)

Supervisor panel (Qt + React/Tauri) and bridge exposure of
`SupervisorStatus`; live JEV evaluation of the dataset and recorded
experiments; exposure/FPS as live backend metrics; HDF5 provenance group at
the next schema bump. Bounded single-parameter control needs its own issue
and review gate.
