# Processing Contract v2 — Validation & Release Gate

Status: deterministic portion complete; real-corpus / hardware / MLflow pending.

This is the release gate for the Processing Contract v2 epic
([#296](https://github.com/KPT1020/mib-studio-qt/issues/296), issue V2-7 #303).
See [ADR 0001](decisions/0006-processing-contract-v2.md) and the
[compatibility matrix](architecture/processing-contract-compatibility.md).

## Calibration decisions

- **The Laplacian-variance focus gate ships disabled.** `ProcessingConfig`
  defaults `enable_laplacian_variance_check = false` with placeholder
  `laplacian_variance_min/max = 0`. No threshold was derived by converting the
  old ring-ratio range — the two metrics are not comparable. Thresholds will be
  set only from the calibration experiments below and are reviewed before the
  gate is enabled.
- **Preprocessing defaults are identity.** The v2 filter chain is a no-op until
  optional filter defaults are calibrated; the identity chain is byte-identical
  to the omitted baseline.
- **Contract 1 is unchanged.** v2 is a coexisting, per-document/per-core
  contract; the global `contract_version` is not bumped.

## Deterministic synthetic conformance (verified here)

Reproducible, platform-independent fixtures assert the v2 properties. These run
in the backend-only CTest lane:

| Property | Test |
|---|---|
| Absolute-difference polarity symmetry; subtract is polarity-sensitive | `processing.contract2_conformance`, `processing.image_filter_pipeline` |
| Filter identity == baseline; deterministic ordering; fail-closed on bad stages | `processing.image_filter_pipeline` |
| Mask + empty-frame share one difference helper | `processing.image_filter_pipeline` |
| Per-object Laplacian variance: blur lowers score, inversion preserves it, neighbours/background excluded, NaN for tiny/no object, per-object independence | `processing.laplacian_variance`, `processing.contract2_conformance` |
| Incompatible non-empty background is a Contract-2 error | `processing.contract2_conformance` |
| Focus gate disabled by default (no silent invalidation) | `processing.contract2_conformance` |
| Config schema/migration + compatibility matrix | `processing.contract_v2_migration` |
| Engine ABI v2 POD conformance + capability negotiation | `processing.core_abi_v2_c`, `processing.core_capabilities` |
| Focus-score autofocus convergence/stability/clamp | `backend.autofocus_focus_score` |
| Contract-aware gold-standard metrics schema | `scripts.gold_standard_schema_contract` |
| Contract-1 golden output unchanged | `processing.science_golden`, `processing.science_seam`, `processing.core_abi_c` |

## Pending — requires resources not available in CI/container

These acceptance items need the approved real corpus, hardware recordings, an
MLflow server, and the signed native v2 plugin. They are **not** satisfied by
this slice and remain open on the release gate:

- [ ] Controlled experiments over the approved real corpus + hardware
      recordings; upload original/filtered/blurred/diff/threshold/mask/object
      crops/overlays/params/metrics/summaries to MLflow (`mlflow.yofo.bio`).
- [ ] Calibrate the Laplacian gate, optional filter defaults, and autofocus
      sample/step parameters from that evidence; keep the gate disabled until
      explicitly reviewed.
- [ ] Establish v2 golden/conformance references (with fixture + core
      provenance) for the native and bundled cores and the Python wheel; prove
      native/bundled/Python equivalence.
- [ ] Realtime/batch latency, allocation, queue, and frame-accounting budgets
      on representative data; stress / watchdog / TSan / fault-injection /
      long-run hardware autofocus.
- [ ] Contract-1 downgrade and legacy reanalysis on real files.
- [ ] The native v2 plugin (`get_api_v2`) and loader v2 activation from V2-5,
      plus the HDF5 compound migration and review/monitoring UI from V2-6.

## Provenance

Deterministic fixtures are generated in-test from fixed seeds/patterns (no
external data). When real-corpus references are produced, record dataset
revision, frame windows, config, and core identity (version, ABI, contract,
artifact/manifest digests) alongside each reference.
