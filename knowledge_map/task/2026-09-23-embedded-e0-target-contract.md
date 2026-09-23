# Embedded E0 — Zynq-7035 target, ownership, budgets and reuse inventory (#443)

Issue: https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/443 (epic #441).
Board counterpart: https://github.com/gavinlouuu-kpt/pz7035-imx426/issues/4.
Plan: `docs/exec-plans/active/2026-09-23-embedded-e0-target-contract.md`.
Decision: `docs/decisions/0006-standalone-zynq7035-target-and-ownership.md`.
Related notes: [[architecture/AppBackend]], [[architecture/ExperimentCoordinator]],
[[services/DeviceDiscoveryService]], [[build-and-run/Build]].

## What changed

- **Manifest** `deploy/embedded/pz7035-target.json` with schema
  `pz7035-target.schema.json`. Sections: pinned application/board baselines,
  `target` (SoC, PS, board, memory, storage, boot, runtime, toolchain,
  display, peripherals, `excluded_targets`), `boundaries` (native plugin
  ABI v1 / local IPC planned / PS–PL wire board-owned), `ownership` (19
  rows, one owner each: PL, PS or UI), `budgets` (19 entries with
  `frozen|proposed|sizing-input|unknown`, derivation, overflow policy,
  owner), `reuse` (capability → code → tests → evidence → gap → owner),
  `dependency_allowlist` (ARM runtime / host reference tests / touchscreen /
  forbidden), `unknowns` U1–U17 with owners and what they do and do not block.
- **Validator** `scripts/check_embedded_target.py` (stdlib only): JSON
  Schema subset (`$ref`, type, required, additionalProperties, properties,
  items, enum, const, minimum, pattern) plus E0 rules — ARMv7/32-bit target,
  no excluded-target tokens inside `target`/`dependency_allowlist`, every
  `ctest:<name>` cited exists in `tests/CMakeLists.txt`, unresolved ownership
  rows are backed by an unknown, budgets are numbers or explicit unknowns
  (never zero), unknown ids unique and referenced, forbidden dependencies
  absent from the ARM allowlist. Registered as `scripts.embedded_target_manifest`
  and as a `docs-ci.yml` step.
- **No-UI fixture** `tests/integration/e2e_headless_experiment_smoke_test.cpp`
  (`integration.e2e_headless_experiment_smoke`, labels
  `integration;e2e;recording;hdf5;embedded`). Facade-driven: explicit mock
  source → readiness fails closed (`camera.session` Fail, `camera.source`
  Warn, candidate `simulated && !fallback`) → StartCapture → readiness Pass →
  stale generation refused with no side effects → Start with profile id →
  duplicate Start refused → wait for ≥ 40 admitted frames
  (`ProcessingService::experimentAccountingSnapshot().admitted`) → Stop →
  terminal Idle, `finalizationOk`, completion `Complete`, file closed →
  reopen: run-snapshot JSON equals the frozen snapshot, readiness JSON
  present, accounting `Complete`/reconciled/no undeclared loss and equal to
  the live snapshot, readable frames == `persistenceCommitted`,
  `experiment_info` totals never exceed readable frames (TD-16), admitted
  frames conserved across outcomes, core identity matches. Uses `MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL=file:///nonexistent`
  and `MIB_CAMERA_MODE=mock` so nothing touches the network or hardware.
- **Support** `tests/support/wait.h`: `mib::test::waitFor(pred, timeoutMs, pollMs)`.

## Decisions worth remembering

- Fixed-count means "at least N admitted, then client Stop". There is no
  exact-count terminator in the backend; `loopFiles=false` was rejected
  because the mock camera then ends the stream and the capture session
  faults (`StreamEnded`), which is not the appliance Stop path. Exact-count
  admission is #446's step 3.
- Tracking/target/gating: PS is the *provisional* owner, status
  `unresolved`, until its A9 cost is measured (U12). Missing PL capability
  is a readiness failure, never a software fallback.
- Proposed budgets (256 MiB max single allocation, 10 Hz preview, 100 ms
  touch, 60 s boot-to-ready, 10 s cancellation bound, 10k chart points) are
  E0 proposals with derivations; E1/E2/E4/E5 measure and revise them.

## Gaps handed to other slices (recorded, not fixed)

| Gap | Owner |
|---|---|
| Exact-count admission terminator | #446 |
| Native-core artifact naming only knows `x86_64`/`aarch64` (`src/backend/CMakeLists.txt`) | #445 |
| `ExperimentFrameBuffer::Policy.maxBytes` defaults to 0 (no byte cap) | #445 |
| Appliance host must set startup-discovery and LUT-cache policies explicitly | #446 |
| `/experiment_info` totals are remainder-flush counts, not run totals (TD-16); accounting attributes are correct | recording owner (#371) |
| Every hardware unknown U1–U17 | board PZ1–PZ4, E4, E5 |

## Evidence class of this change

Source review + x86_64 Linux compilation/test (`linux-backend-only`,
MindVision off). No ARM build, emulation, synthesis or physical evidence.
