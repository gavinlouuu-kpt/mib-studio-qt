# Contract 2 safe rollout (absdiff + Laplacian variance)

Status: active. Phase 0 merged 2026-09-26 (#455, #457, #458); T1.2 done
(real-frame Contract 2 reference, #459). T1.1 split: T1.1a (loader + core
owns Contract-2 science) done 2026-09-27; T1.1b (release pipeline: both core
lines built, audited, signed, published) and T1.1c (Processing Core dialog
shows and filters by contract) next.

Builds on the [Processing Contract v2 plan](2026-07-21-processing-contract-v2.md),
[ADR 0006](../../decisions/0006-processing-contract-v2.md) (what Contract 2
computes) and [ADR 0007](../../decisions/0007-one-contract-per-shipped-core.md)
(one shipped core = one contract). This plan defines how Contract 2 reaches
rigs without putting Contract 1 at risk.

## Goal

Introduce the Contract 2 pipeline (`cv::absdiff` background difference,
top-level contours, per-object Laplacian variance instead of ring width) so
that:

1. The Contract 1 core that rigs run is never rebuilt or re-signed because of
   Contract 2 work, and Contract 1 results stay byte-for-byte identical on
   synthetic **and** real frames.
2. A rig runs Contract 2 only by activating a signed Contract 2 core, and only
   with a Contract 2 profile. Any other combination is refused.
3. Every recording says which core, and therefore which contract, produced it.
4. Rollback is activating the previous core in the Processing Core dialog.

Non-goals: changing the default contract, retiring Contract 1, or new
algorithms beyond Contract 2.

## Model

```
          one codebase (mib-studio-qt)
                     │
     ┌───────────────┼──────────────────────┐
     ▼               ▼                      ▼
subtract-ring     absdiff-laplacian    Python wheel (research only)
core (Contract 1) core (Contract 2)    runs either contract per call
bundled in app    signed plugin only   for offline comparison
+ signed plugin   (needs V2-5)
     │               │
     └── profile's processing_contract_version must EQUAL
         the active core's contract, or processing is refused
```

- **The contract** is the science: what the numbers mean. It is a fixed
  property of a core build.
- **The core version** is the build: fixes, performance, new non-science
  outputs. Each contract has its own core release line, named after its
  algorithm: `subtract-ring` (Contract 1) and `absdiff-laplacian`
  (Contract 2).
- **The profile** states which contract it needs. It does not choose one.

## Protection model

| # | Safeguard | Protects against |
|---|-----------|------------------|
| P1 | **Explore outside shipped cores.** New methods run in the wheel, notebooks, or an unsigned experimental core. | Experiments reaching rigs |
| P2 | **One shipped core = one contract** (ADR 0007). New science means a new contract and a new core line; released lines are never repurposed. | Contract 2 work changing the Contract 1 binary; mismatched labels |
| P3 | **Frozen gold references per contract**, checked against every build of that contract's core. References change only through `--update-reference` in a PR labelled `gold-reference-change`. | Shared-code edits changing either contract's output |
| P4 | **Pinned, signed core per rig**, promoted only after passing its contract's references. | Untested code reaching a working rig |
| P5 | **Provenance from the core**: contract, core version and SHA-256, plus the full processing config, in every recording. | Results that cannot be traced or reproduced |

## Current state (verified on `feat/pc2-contract2`, PR #452)

Reusable as-is:

- The science library executes both contracts, selected by
  `ProcessingConfig::processing_contract_version` through the predicates in
  `include/backend/processing/ProcessingContract.h`. There is one shared
  difference step, `differenceImage()`
  (`src/backend/processing/ImageFilterPipeline.cpp`).
- The native loader already refuses a descriptor whose contract differs from
  `ProcessingCoreLoadRequirements::expectedContractVersion`
  (`ProcessingCoreLoader.cpp:476`; the field is fixed at `1` in
  `ProcessingCoreLoader.h`). The compatibility matrix already lists a
  profile/core contract mismatch as "refuse".
- Processing Core dialog: signed registry cores, a content-addressed cache,
  downgrade confirmation, refusal while running, and an administrator pin.
  Verified 0.2.0 → 0.1.0 on Windows on 2026-07-15.
- Recordings store `processing_core_version`, `processing_core_sha256`,
  `processing_engine_abi_version` and `processing_contract_version`, all from
  the active core identity.
- Contract 1 gold reference (`scripts/gold_standard_dataset.json`, 2 synthetic
  frames), which matched 2/2 on 2026-09-25. Contract 2 has deterministic
  synthetic conformance tests (`processing.contract2_conformance`). The
  Laplacian gate ships disabled.

Gaps:

| Gap | Detail | Resolved by |
|-----|--------|-------------|
| G1 | The bundled kernel obeys the profile's contract (V2-8 runtime selection), while recordings stamp the core's contract (always 1). A Contract 2 run in the app is recorded and exported as Contract 1. | T0.1, T0.2 |
| G2 | Native core contract is a hard-coded `1` everywhere: `MIB_PROCESSING_CONTRACT_VERSION`, the descriptor, the loader expectation. There is no way to build or load a Contract 2 core. | T0.1, T1.1 |
| G3 | The recorded `processing_config_*` attributes omit the difference threshold, Laplacian gate and channel band. | T0.2 |
| G4 | Contract 1 gold is 2 synthetic frames: no real cells, walls or defocus. | T0.3 |
| G5 | No Contract 2 gold reference. | T1.2 |
| G6 | Nothing flags a PR that edits frozen stage code or a gold reference. | T0.4 |
| G7 | Contract 2 plugin loading and signing is not active (V2-5). Under ADR 0007 this is the only way Contract 2 reaches a rig. | T1.1 |
| G8 | The UI doesn't show the active contract; Contract 2 labels still say "ring". | T3.1 |
| G9 | Realtime cost of Contract 2 on the sorting path is unmeasured. | T2.3 |

## Phases and gates

Nothing moves to the next phase until its exit gate passes.

### Phase 0: Enforce one contract per core; lock Contract 1

- **T0.1 Build-time contract, enforced everywhere** (G1, G2).
  - A CMake option `MIB_PROCESSING_CORE_CONTRACT` (1 | 2, default 1) sets the
    core identity's contract, the native descriptor's `contract_version`, the
    artifact name, and the loader's `expectedContractVersion`.
  - The bundled kernel and the native plugin refuse a config whose
    `processing_contract_version` differs from their build contract. They return
    an error and produce no mask.
  - `ProcessingService` refuses to start realtime, batch or HDF5 reanalysis when
    the profile's contract differs from the active core's contract. It reports
    both numbers to the UI.
  - The wheel keeps per-call selection (research, ADR 0007 point 4).
  - Tests: a Contract 2 profile on a Contract 1 core is refused on each path;
    Contract 1 gold is unchanged.
- **T0.2 Provenance completeness** (G1, G3). The recorded contract stays the
  core's (now correct by construction). Add the profile's declared contract,
  `difference_threshold`, the Laplacian gate and the channel band to the
  recorded config attributes. Round-trip test in `recording.experiment_roundtrip`,
  and an exporter test that a Contract 2 core's recording exports
  `laplacian_variance`.
- **T0.3 Real-frame Contract 1 gold** (G4). A fixed set of recorded frames
  (walls, cells, defocus, empty frames) run through the current Contract 1
  core and frozen. The conformance harness checks it on every Contract 1 build.
- **T0.4 Change control** (G6). CODEOWNERS on `ProcessingContract.h`, the
  `differenceImage` path, the object evaluators in `ProcessingScience.cpp`,
  and the gold reference files. A `gold-reference-change` label is required for
  any PR that moves a reference.

Exit gate: T0.1–T0.4 merged. Both Contract 1 references pass on Linux, Windows
and the wheel. The mismatch refusal is covered by tests.

### Phase 1: Contract 2 core line

- **T1.1 Contract 2 plugin core** (G2, G7).
  - Build `mib_processing_core` with `MIB_PROCESSING_CORE_CONTRACT=2` and ABI v2.
  - Line names per ADR 0007: `subtract-ring` (Contract 1) and
    `absdiff-laplacian` (Contract 2). Release tags
    `mib-processing-<name>-v<semver>` (for example
    `mib-processing-absdiff-laplacian-v0.1.0`). Artifacts
    `mib_processing_core-<name>-<semver>-<os>_<arch>`. The descriptor gains
    `algorithm: "<name>"` next to `contract_version`.
  - The wheel/core CI trigger (`tags: ['mib-processing-v*']` in
    `python-wheel.yml`) and the tag-to-version check accept both line
    prefixes. The existing `mib-processing-v0.1.0` and `v0.2.0` releases stay
    as legacy `subtract-ring` releases; the registry and R2 objects are
    immutable.
  - Activate V2-5: the loader accepts an ABI v2 descriptor whose contract equals
    the profile's.
  - Sign and publish to the registry, which exposes the contract per artifact.
  - The Processing Core dialog shows the contract and only offers cores that
    match the active profile's contract.
- **T1.2 Contract 2 gold references** (G5). The synthetic C-series fixture,
  plus a real subset of the focus dataset (every cell line, 40–70 V), with
  expected masks, objects, `laplacian_variance` and `in_channel`. Checked
  against every Contract 2 core build and the wheel.
- **T1.3 Release the Contract 2 definition.** Mark it released in ADR 0006,
  including the top-level-contour rule and the channel-band gate. From then on,
  any change to its output is Contract 3 with its own core line.

Exit gate: a signed Contract 2 core passes the Contract 2 references on Linux
and Windows. The Contract 1 core binary and its references are unchanged.

### Phase 2: Offline comparison (no rig changes)

- **T2.1** Re-analyse existing Contract 1 recordings with Contract 2 in the
  wheel. Report per recording: objects found, valid counts, focus metric vs
  voltage, and cells accepted by one contract but not the other.
- **T2.2** (V2-7) Calibrate the Laplacian gate and the difference threshold. If
  no threshold separates in-focus from out-of-focus reliably, the gate stays
  disabled.
- **T2.3** (G9) Measure the latency and throughput of the Contract 2 core on rig
  hardware (sorting-trigger path, `scripts/analyze_pipeline_timing.py`),
  against the Contract 1 core.

Exit gate: comparison report reviewed; thresholds chosen, or the gate kept off;
latency within budget.

### Phase 3: Opt-in on rigs

- **T3.1** (G8) The UI shows the active core by line name and contract (for
  example "Absdiff + Laplacian (Contract 2) 0.1.0"). Under Contract 2
  it labels the focus metric "Laplacian variance" and hides ring controls. A
  mismatch between profile and core is explained, never silently fixed.
- **T3.2** Pilot rig: activate the signed Contract 2 core, load a Contract 2
  profile, and apply the administrator pin. Other rigs stay on the Contract 1
  core.
- **T3.3** Rollback drill before the pilot: activate the previous core and the
  Contract 1 profile, then confirm the next recording carries Contract 1
  provenance.

Exit gate: operator sign-off after the agreed pilot runs, with no provenance or
latency regressions.

### Phase 4: Default switch and Contract 1 retirement (separate decision)

Out of scope. When it happens: stop new Contract 1 releases, keep the last
Contract 1 core signed and available, and keep its references.

## Change policy (from Phase 0 on)

- **New science** means a new contract number, a new core line, and new
  references. A released line is never repurposed.
- **Shared-code changes** rebuild every maintained core line. Each line must
  still match its references exactly. Moving a reference needs
  `gold-reference-change` and processing-owner approval.
- **New ideas** are prototyped in the wheel first, as the channel gate was
  (`scripts/channel_roi.py` → `detectChannelRoi`).

## Effect on PR #452

PR #452 can merge as is. Its runtime selector stays in the science library,
where the wheel uses it, and every profile defaults to Contract 1. T0.1 then
takes the ability to run Contract 2 away from the desktop app's bundled
kernel. **No rig profile should be set to Contract 2 before T0.1 lands.** Until
then, a Contract 2 recording would carry the wrong contract (G1).

## Open decisions (Gavin)

1. Which recordings make up the real-frame references (T0.3, T1.2).
2. Latency budget for the sorting path under Contract 2 (T2.3).
3. Pilot size and sign-off owner (Phase 3).

## Task summary

| Task | Gaps | Size | Blocks |
|------|------|------|--------|
| T0.1 build-time contract + refusal on mismatch | G1, G2 | M | any rig use of Contract 2 |
| T0.2 provenance completeness | G1, G3 | S | Phase 0 exit |
| T0.3 real-frame Contract 1 gold | G4 | S | Phase 0 exit |
| T0.4 CODEOWNERS + reference label | G6 | S | Phase 0 exit |
| T1.1 signed Contract 2 plugin core (V2-5) | G2, G7 | L | Phase 1 exit |
| T1.2 Contract 2 gold (synthetic + real) | G5 | M | Phase 1 exit |
| T1.3 release Contract 2 definition | — | S | change policy |
| T2.1–T2.3 comparison, calibration, latency | G9 | M–L | Phase 2 exit |
| T3.1–T3.3 UI, pilot, rollback drill | G8 | M | Phase 3 exit |
