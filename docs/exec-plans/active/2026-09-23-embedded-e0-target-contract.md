# Embedded E0 — Zynq-7035 target, ownership, budgets and reuse inventory (#443)

Status: active

Issue: https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/443 (epic #441)
Board counterpart: https://github.com/gavinlouuu-kpt/pz7035-imx426/issues/4 (PZ1)
Baseline: `develop` at `2fe02824de3a72114b7c7d5a3a2af46a6cc64b95` (application),
`pz7035-imx426` `main` at `3b782da75c358f7efd357936bd7026e36371e93d` (board), both reviewed 2026-09-23.
ADR: [0006 — Standalone Zynq-7035 target, ownership and boundaries](../../decisions/0006-standalone-zynq7035-target-and-ownership.md)
Manifest: [`deploy/embedded/pz7035-target.json`](../../../deploy/embedded/pz7035-target.json)

## Goal

E1–E5 can start from one reviewed, machine-checked platform contract: the
exact target and its unknowns, a one-owner PS/PL/UI matrix for every
standalone-required operation, numeric budgets with status and overflow
policy, the reuse/dependency inventory, and a deterministic no-UI experiment
regression over the real backend. Nothing in this plan builds for ARM, adds
firmware, or claims hardware acceptance.

## Evidence classification used throughout

`source-review` · `compilation` · `emulation` · `simulation` · `synthesis` ·
`target-software-test` · `physical-acceptance` · `vendor-documentation` ·
`unknown`. Every claim in the manifest names one. This PR's own evidence is
**source review** plus **compilation and test on x86_64 Linux** (the
`linux-backend-only` preset); it contains no ARM or board evidence.

## Acceptance criteria (from #443)

- [x] Target and display requirements explicit; no AArch64, KU5P/MicroBlaze,
      GPU or unselected-board assumptions in the baseline — enforced by
      `scripts/check_embedded_target.py` (`target.excluded_targets`).
- [x] One-owner PS/PL/UI matrix covering standalone-required operations and
      failure paths — `ownership[]`, 19 rows, each with failure policy and test.
- [x] Required capabilities and resource/deadline/recording policies are
      measurable with unresolved hardware gates named — `budgets{}` with
      status/derivation/overflow policy; `unknowns[]` U1–U17 with owners.
- [x] Implemented vs remaining work distinguished by source/tests — `reuse[]`
      cites code paths and registered CTest names (validator checks they exist).
- [x] Desktop/no-Qt regression fixture and manifest validation run without
      hardware or cloud access — `integration.e2e_headless_experiment_smoke`,
      `scripts.embedded_target_manifest`, both in the linux-backend-only lane.
- [x] Handoff enables parallel E1/E2/E3-fake/E4-simulator work — see
      "Handoffs" below.

## Decision log

- 2026-09-23: Manifest lives under `deploy/embedded/` (deployment/target
  contract, alongside signing/sentry deployment inputs), not `env/` (host
  setup lists) or `crates/mib-bridge/contract/` (in-process bridge ABI).
- 2026-09-23: The validator is stdlib-only (same policy as
  `scripts/test_export_hdf5_paths.py`); no `jsonschema` dependency enters the
  CTest lanes. It implements the schema subset the manifest uses plus the
  semantic rules the schema cannot express.
- 2026-09-23: The no-UI fixture stops the run after **at least** N admitted
  frames rather than exactly N. The backend has no exact-count admission
  terminator; adding one is #446's "fixed admitted frame count/range" step,
  not an E0 change. `MockCameraOptions.loopFiles=false` was rejected as the
  terminator because the camera then ends the stream and the capture session
  faults (`CaptureFailureKind::StreamEnded`), which is not the appliance's
  normal Stop path.
- 2026-09-23: Tracking/target/gating gets PS as *provisional* owner with
  status `unresolved` (U12). The epic requires an explicit assignment after
  measurement; recording the decision point is the honest E0 output.
- 2026-09-23: Proposed (not frozen) numbers — 256 MiB max single allocation,
  10 Hz preview, 100 ms touch response, 60 s boot-to-ready, 10 s
  cancellation bound, 10k chart points — are E0 proposals with derivations
  so E1/E2/E4/E5 have something concrete to measure against and revise.

## Progress

- [x] ADR 0006 and this plan.
- [x] `deploy/embedded/pz7035-target.json` + schema + `scripts/check_embedded_target.py`,
      registered as `scripts.embedded_target_manifest` (CTest) and run by
      `docs-ci.yml`.
- [x] `tests/integration/e2e_headless_experiment_smoke_test.cpp` +
      `tests/support/wait.h`, registered as `integration.e2e_headless_experiment_smoke`.
- [x] Vault: task note, Recent-Work, decisions index, docs index, Build note.
- [ ] Reviewer pass on the ownership rows and proposed budgets (owner: epic
      owner; board rows co-reviewed against PZ1).
- [ ] Board PZ1 fills U1–U11/U16 with immutable references; MIB re-reviews
      the manifest and bumps `reviewed`.

## Handoffs (what each slice takes from E0)

| Slice | Takes | Must return |
|---|---|---|
| E1 #445 | `target.processing_system`, `dependency_allowlist.arm_runtime`, `forbidden_in_arm_runtime`, budgets `ps_address_bits`, `max_single_allocation_bytes`, `frame_store_ring_frames`, `experiment_buffer_max_frames` | ARMv7 build profile, `linux_armv7` native-core naming/fingerprint, emulation vs native evidence labelled separately, `process_rss_budget_bytes` once DDR (U2) is known |
| E2 #446 | ownership rows for readiness/lifecycle/discovery/client-loss, `boundaries.local_application_ipc`, the no-UI fixture, budget `cancellation_bound_ms` | headless host + local API + CLI reusing the fixture; exact-count admission terminator; controller-loss Stop & Save regression; IPC version in `boundaries` |
| E3 #447 | `boundaries.ps_pl_wire` (board PZ2), FPGA result ingestion row, `result_burst_capacity_records` (U13), tracking placement (U12) | fake/replay provider tests first; measured A9 tracking cost; loss accounting per class; version/capability negotiation |
| E4 #448 | `target.display`, UI ownership rows, budgets `preview_rate_hz_max`, `touch_response_ms`, `chart_history_max_points`, `dependency_allowlist.touchscreen` | API-backed simulator; on-target renderer selection by measurement; combined-load budget evidence; display/touch unknowns (U6, U7) closed with references |
| E5 #449 | `target.storage/boot`, budgets `boot_to_ready_s`, `fault_response_ms`, `storage_write_bytes_per_second`, `full_raw_recording_admission` | reproducible offline image; watchdog/fault/update evidence; physical no-PC acceptance; U3, U4, U8, U14 closed |
| Board PZ1 (pz7035-imx426#4) | the whole `target` section and `unknowns` list | revision, DDR, storage, boot, BSP/toolchain, display/touch, peripheral endpoints, PS clock, operating-mode declarations with hashes |

## Verified failures recorded for narrow follow-up (not fixed here)

| Finding | Owner | Where recorded |
|---|---|---|
| No exact-count run terminator; runs end by client Stop or camera stream end | #446 | manifest `reuse[]` "Mock camera source"; decision log above |
| `src/backend/CMakeLists.txt` names native cores only for `x86_64`/`aarch64`; an ARMv7 build would emit `linux_arm` or `linux_armv7l` unreviewed | #445 | manifest `reuse[]` "Processing-core ABI" |
| Startup discovery/LUT-cache policies are shell-supplied; the appliance host must set them explicitly | #446 | manifest ownership row "Device discovery" |
| `ExperimentFrameBuffer::Policy.maxBytes` defaults to 0 (no byte cap) | #445 | manifest budget `experiment_buffer_max_frames` |
| `/experiment_info` `total_valid_frames`/`total_invalid_frames` are the remainder-flush counts, not run totals (fixture saw 41 committed, totals 0); `accounting_*` is the truth | #371 owner (recording) | tech-debt tracker TD-16; fixture asserts `<=` with a TD-16 comment |

## Non-goals (unchanged from #443)

Board procurement/design, a new FPGA scientific algorithm, an RTL port, a
replacement experiment coordinator or a new UI. #423 remains the
scientific/FPGA-feasibility gate.
