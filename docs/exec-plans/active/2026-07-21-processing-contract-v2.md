# Processing Contract v2

Status: active

## Goal

Ship Processing Contract v2 — a new, explicitly versioned scientific pipeline
that uses `cv::absdiff` for background comparison, adds a deterministic
preprocessing filter stage, and replaces ring width with a per-detected-object
Laplacian variance focus metric — while keeping Contract v1 byte-for-byte
reproducible. Every profile, native core, Python wheel, and HDF5 file declares
and is matched on its contract, and legacy artifacts remain readable without
presenting ring width as a current v2 metric.

GitHub tracking:

- [Epic #296](https://github.com/KPT1020/mib-studio-qt/issues/296)
- V2-1 [#297], V2-2 [#298], V2-3 [#299], V2-4 [#300], V2-5 [#301],
  V2-6 [#302], V2-7 [#303]
- Builds on hot-swappable cores epic [#236] and [#242].

See [ADR 0001](../../decisions/0006-processing-contract-v2.md) and the
[compatibility matrix](../../architecture/processing-contract-compatibility.md).

## Acceptance criteria (epic)

- [ ] Contract 1 golden outputs are byte-for-byte unchanged.
- [ ] Contract-1 and Contract-2 profiles execute only with compatible
      implementations.
- [ ] Existing schema-1 files are never silently rewritten with schema-2 keys.
- [ ] Copy-upgrade preserves unrelated values and removes all live ring config.
- [ ] v2 uses `cv::absdiff` and one shared difference path across realtime,
      batch, HDF5 reanalysis, empty filtering, and auto-background.
- [ ] Laplacian variance is computed per detected object only; N objects → N
      independent values; masking applied to statistics, not before convolution.
- [ ] v2 autofocus maximizes an object focus score; no ring terminology.
- [ ] ABI v1 and v2 coexist and negotiate explicitly.
- [ ] HDF5/Python/C++/JSON/CSV agree on names, units, ordering, NaN handling.
- [ ] Calibrated defaults with the Laplacian gate disabled until reviewed.

## Delivery slices and dependencies

| Slice | Issue | Depends on | Summary |
|-------|-------|-----------|---------|
| V2-1 | #297 | — | Schema, versioning, migration, compatibility boundary |
| V2-2 | #298 | V2-1 | Preprocessing filters + shared absdiff difference path |
| V2-3 | #299 | V2-1, V2-2 | Remove ring width; per-object Laplacian variance |
| V2-4 | #300 | V2-3 | Focus-score autofocus controller |
| V2-5 | #301 | V2-1, V2-2, V2-3 | Engine ABI v2 (filters + full per-object results) |
| V2-6 | #302 | V2-1, V2-3, V2-5 | HDF5/Python/export/profiles/UI migration |
| V2-7 | #303 | V2-2…V2-6 | Calibration + validation (release gate) |

Branches are stacked in this order (`claude/pc2-v2-1-schema` → … ).

## Decision log

- 2026-07-21: Two version axes (`processing_contract_version`,
  `config_schema_version`), both bumped to `2` for v2; matched by equality, not
  ordering (ADR 0001).
- 2026-07-21: v2 migration logic lives Qt-free in `mib_processing`
  (`backend::processing::contract`, nlohmann::json) so it is exercised by the
  backend-only CTest lane; the Qt `ProfileManager` calls into it.
- 2026-07-21: Canonical v2 difference key is `difference_threshold`;
  `bg_subtract_threshold` is accepted only through the compatibility adapter.
- 2026-07-21: Migrated profiles get an identity preprocessing chain and a
  disabled Laplacian gate; the gate stays disabled until calibrated in V2-7.

## Progress

### V2-1 — schema, compatibility, migration boundary
- [x] ADR 0001 + compatibility-matrix doc.
- [x] Qt-free contract module: version constants, schema classifier, canonical
      difference-threshold adapter, v1→v2 migrator.
- [x] Backend unit tests (migration, canonical/legacy key, classifier,
      fail-closed).
- [x] Schema-aware `ProfileManager` loading (`normalizeConfigForSchema` fails
      closed on unknown schema; `copyUpgradeConfigToV2` bridges to the migrator).
- [x] Vault notes updated; `check_docs.py` green.

### V2-2 — preprocessing filters + shared absdiff path
- [x] Qt-free `ImageFilterPipeline` (identity/invert/linear_contrast/gamma/
      clahe), compiled once, fail-closed on unknown/invalid stages.
- [x] One shared `buildDifferenceImage` (+ cropped variant): input filters
      symmetric, contract-gated absdiff vs subtract, difference filters,
      incompatible-background error under v2, ROI zero-outside preserved.
- [x] Bundled kernel `processMask` + `isEmpty` and host `isFrameEmpty` helpers
      routed through the one helper. Contract-1 output unchanged.
- [x] Tests (`processing.image_filter_pipeline`) + vault. Golden/seam/
      multi-object regression green.
- [ ] Real preprocessing stages fed from a v2 config/ABI (deferred to V2-5/V2-6;
      pipelines are identity until then).

### V2-3 — abolish ring width; per-object Laplacian variance
- [x] `science::calculateLaplacianVariance` (filled mask, bbox+kernel crop,
      Laplacian on the unmasked crop, `meanStdDev` masked variance, `NaN` for
      unusable). Computed once per emitted object from its own contour (inner
      for nested, top-level for outer-only).
- [x] `FilterResult::laplacianVariance` + config
      `laplacian_variance_min/max` + `enable_laplacian_variance_check`
      (AppConfigWatcher + Python bridge). `InvalidReasonCode::Laplacian`
      (histogram 6→7).
- [x] Gate disabled by default; Contract-1 output unchanged (golden/seam/
      multi-object/identification-metrics green). Ring width retained for v1.
- [x] Test `processing.laplacian_variance` + vault.
- [ ] Removing ring from the v2 *surface* (HDF5/exports/UI) lands in V2-6; here
      ring stays computed for v1 and the new field/gate are additive.

### V2-4 — focus-score autofocus controller
- [x] Qt-free `AutofocusFocusScore.h`: `FocusSample`, validity policy,
      `medianFocusScore` (dedup by frame/identity, `NaN` when empty), and the
      maximize-score `FocusScoreController` (probe/reverse/refine/hold, clamped).
- [x] Contract-1 setpoint controller (`AutofocusMath.h`) untouched.
- [x] Test `backend.autofocus_focus_score` (converge both sides, plateau/noise
      stable, clamp, sample validity + dedup) + vault.
- [ ] `AutofocusService` `onFocusSample` feed + contract-gated controller
      selection + focus-score UI rename (rides V2-6's config/contract plumbing).

### V2-5 — engine ABI v2 for filters and full per-object results
- [x] Additive ABI v2 in `ProcessingCoreAbi.h` (v1 layout unchanged, pinned by
      `core_abi_c`): POD filter-chain / v2-config / per-object-metrics structs,
      host-owned object buffer with deterministic `BUFFER_TOO_SMALL`,
      `mib_processing_api_v2` + `get_api_v2`.
- [x] Capability flags + `ProcessingCoreCapabilities.h` host negotiation
      (`coreSatisfiesContract2`, `abiV1ServesContract`, `engineAbiForContract`).
- [x] Tests `processing.core_abi_v2_c`, `processing.core_capabilities`.
- [x] Native v2 plugin: `mib_processing_core` exports `mib_processing_get_api_v2`
      whose `process_objects` compiles the filter chain, builds the absolute
      difference, runs the science, and returns full per-object metrics
      (finite Laplacian variance) with deterministic `BUFFER_TOO_SMALL`. The v1
      `get_api` export is unchanged. End-to-end dlopen test
      `processing.core_v2_plugin`.
- [ ] Loader v2 activation path (`DynamicProcessingKernel` v2 / negotiate
      `get_api_v2` through the trust+lease machinery) + native signing — the ABI
      and a working plugin now exist; wiring them through the signed loader and
      packaging remains follow-on (intersects hardware/signing).

### V2-6 — migrate persisted/UI surfaces to Contract 2
- [x] Contract-aware gold-standard metrics schema: `ring_ratio` optional
      (documented legacy Contract 1), new optional `laplacian_variance`
      (Contract 2, `NaN`→`null`); both contracts validate under
      `additionalProperties:false`. No global `contract_version` bump.
- [x] `gold_standard_metrics.md` updated. Test
      `scripts.gold_standard_schema_contract`.
- [x] HDF5 compound round-trips `laplacianVariance` (appended member; a
      Contract-1 file with no member reads as `NaN`). Round-trip test in
      `recording.experiment_roundtrip`. (Contract already recorded via
      `processing_contract_version`; preprocessing-config metadata + reader
      selection remain follow-on.)
- [x] Python JSON exporter is contract-aware: emits `laplacian_variance` and
      omits `ring_ratio` for a Contract-2 file; keeps ring for Contract 1. e2e
      test `scripts.contract2_export_review` (generate-shaped records → export →
      schema-valid v2 review document). CSV exporter unchanged (follow-on).
- [ ] Review/monitoring models, focus-score histograms, invalid reasons,
      config controls, legacy-labeling, mismatch warnings, screenshots/docs —
      follow-on (frontend, needs the running app).

### V2-7 — calibration & validation (release gate)
- [x] Deterministic synthetic conformance gate
      (`processing.contract2_conformance`) tying together polarity symmetry,
      filter identity/order, blur→lower focus, inversion-preserves, object
      isolation, tiny/no-object NaN, invalid-background rejection, and
      focus-gate default-off.
- [x] `docs/processing-contract-v2-validation.md`: calibration decisions (gate
      ships disabled; no threshold converted from ring), the deterministic
      coverage matrix, and the explicit pending list.
- [ ] Real-corpus + hardware experiments, MLflow uploads, calibrated
      thresholds, native/bundled/Python conformance references, latency/stress/
      TSan/fault-injection/long-run, Contract-1 downgrade on real files —
      require the approved corpus, hardware, MLflow, and the signed native v2
      plugin (not available in CI/container). Tracked in the validation doc.

## Status summary

Slices V2-1…V2-7 landed as stacked branches. Every deterministic, container-
verifiable behavior is implemented and tested (Contract-1 output byte-for-byte
unchanged throughout). The remaining work is resource-dependent and enumerated
in `docs/processing-contract-v2-validation.md`: the native v2 plugin + loader
activation (V2-5), the HDF5 compound + exporters + review/monitoring UI (V2-6),
and the real-corpus/hardware/MLflow calibration + release references (V2-7).
