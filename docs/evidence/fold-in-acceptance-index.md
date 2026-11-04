# E/I/F acceptance evidence index — MIBQT-16

Reviewed: 2026-11-04. Engineering baseline: `49121893578b876ec97e729823349ede840c7276`,
[PR #816](https://github.com/gavinlouuu-kpt/mib-studio-qt/pull/816).
This is a **source/evidence audit**, not a new backend test run or an acceptance sign-off.

## Authority and use

The [backend goals](../architecture/fold-in-backend-goals.md) define all 37 requirements.
The [realignment plan](../exec-plans/active/2026-11-02-fold-in-mechanism-realignment.md)
defines the five-layer acceptance rule. This index connects those requirements to
candidate proof and remaining work; it does not change them.
Project actions, ownership and decisions remain in the
[ENG-MIBQT tracker](https://docs.google.com/spreadsheets/d/1iqQqT9xyGauGKvXBQYDDP1Ia90Wq-Y3v3-U6A1saUTw/edit#gid=100).
Task keys below are navigation, not extra tasks or authorization to merge/actuate.

**37/37 goals indexed; 0 newly accepted by this review.** All rows remain open for
acceptance. “Exists” means source inspected, not tests passed. An unreviewed proof
is a verification gap, not a claim that the implementation does not exist.
The [recorded 2026-11-02 e2e](fold-in-e2e-2026-11-02.md) ran at `b8a776c7`,
not this baseline: its 9 passing scenarios are historical evidence, not 37 accepted
goals. Synthetic files written/read by production code are real-backend evidence,
not representative biological recordings or physical-camera qualification.

## Goal-by-goal evidence and next proof

| Goal | Tracker action(s) | Observed source / recorded evidence | Remaining implementation or proof | Evidence entry point |
|---|---|---|---|---|
| E01 | MIBQT-05,19 | Restart/stale-token scenario recorded in integrated e2e; camera feature facade and discovery tests exist. | Reconcile #809 guards with the integration head; missing-producer/busy/disconnected rig evidence. | [fold_in_e2e.rs](../../crates/mib-bridge-server/tests/fold_in_e2e.rs) |
| E02 | MIBQT-05,08 | Node-map enumeration and SFNC commands are wired; e2e records mock read/write/refusal. | Vendor-tool tree parity, selector restoration and access changes on the actual camera. | [feature_tree_test.cpp](../../tests/camera/features/feature_tree_test.cpp) |
| E03 | MIBQT-08 | Existing capture path is available; no complete goal proof reviewed here. | Raw-value oracle across zoom/ROI/part changes and multipart/channel identity. | [fold-in-backend-goals.md](../../docs/architecture/fold-in-backend-goals.md) |
| E04 | MIBQT-14,08 | E2e records acquisition and reconciled runs; a separate first-frame branch is recorded in the tracker. | Review and integrate MIBQT-14; preview-failure injection must leave writer state unchanged. | [fold_in_e2e.rs](../../crates/mib-bridge-server/tests/fold_in_e2e.rs) |
| E05 | MIBQT-08,19 | Existing retention/playback services; #809 evidence is outside this integration scope. | Exact-ID playback, counted eviction, reserve configuration and stop/reallocate/no-auto-resume proof on the target head. | [2026-11-02-fold-in-mechanism-realignment.md](../../docs/exec-plans/active/2026-11-02-fold-in-mechanism-realignment.md) |
| E06 | MIBQT-08 | E2e records HDF reopen and reconciled accounting for mock-camera runs. | Disk-full/cancel/disconnect/failed-close matrix and exported-range reopen; this is not physical qualification. | [fold-in-e2e-2026-11-02.md](../../docs/evidence/fold-in-e2e-2026-11-02.md) |
| E07 | MIBQT-08,16 | Coordinator readiness tests exist. | Map every required readiness state, storage/compatibility support and no-actuation proof; existing tests alone do not certify coverage. | [experiment_readiness_test.cpp](../../tests/backend/experiment_readiness_test.cpp) |
| E08 | MIBQT-08,16 | Camera-script test exists. | Acquiring/authorization refusal matrix and mock/rig version-capability reports. | [camera_script_apply_test.cpp](../../tests/backend/camera_script_apply_test.cpp) |
| E09 | MIBQT-08,16 | Integrated e2e records 12 gallery tiles with pixels on synthetic data. | Full revision-bearing reference and expired-image proof, plus physical object rows; zero-object capture cannot qualify this. | [evidence-gallery-app-parity.mjs](../../tools/flowjo_acceptance/evidence-gallery-app-parity.mjs) |
| I01 | MIBQT-12 | Workspace persistence exists; end-to-end annotation grouping is not certified by the integrated scenario. | Schema-backed annotation write to grouped F statistics/export; independent experimental units, not cell-count replication. | [workspace_store_test.cpp](../../tests/workspace/workspace_store_test.cpp) |
| I02 | MIBQT-06,15 | Native recipe tests; e2e records immutable create/derive/diff/apply preview and restart. | Production wiring/complete proof review; resolve recorded method-different product decision separately from backend correctness. | [recipe_provider_test.cpp](../../tests/workspace/recipe_provider_test.cpp) |
| I03 | MIBQT-11 | Estimator remains an open implementation slice in the realignment plan. | Native time/storage/resource estimator, assumptions/freshness and estimate-versus-measured tolerance evidence. | [2026-11-02-fold-in-mechanism-realignment.md](../../docs/exec-plans/active/2026-11-02-fold-in-mechanism-realignment.md) |
| I04 | MIBQT-16 | Coordinator run path exists; integrated e2e is not a disconnect/retry identity proof. | Exactly one correctly labelled run/file after disconnect and request-ID retry. | [fold_in_e2e.rs](../../crates/mib-bridge-server/tests/fold_in_e2e.rs) |
| I05 | MIBQT-16 | Monitoring/cell facade tests exist. | Reconnect through production live view with lifecycle/loss fields and exact event resolution. | [monitoring_cells_facade_test.cpp](../../tests/backend/monitoring_cells_facade_test.cpp) |
| I06 | MIBQT-10,16 | Native F results and recipes exist independently. | Reference/run deltas through native authority to a linked Repeat/Adjust/Continue draft. | [recipe_provider_test.cpp](../../tests/workspace/recipe_provider_test.cpp) |
| I07 | MIBQT-10 | Named metrics remain an open implementation slice. | Native definitions, validated expression grammar, invalid-expression refusal and quality flags. | [2026-11-02-fold-in-mechanism-realignment.md](../../docs/exec-plans/active/2026-11-02-fold-in-mechanism-realignment.md) |
| I08 | MIBQT-10 | F population authority exists; I criteria production integration remains open. | Persist criteria/missing policy, independent hit-count oracle and proof edits never deploy a sorting gate. | [population_test.cpp](../../tests/analysis/population_test.cpp) |
| I09 | MIBQT-12 | Native comparison authority and insufficient-unit e2e evidence exist. | I production selection/grouping plus compatible/incompatible source-set exclusion reasons. | [comparison_spec_test.cpp](../../tests/analysis/comparison_spec_test.cpp) |
| I10 | MIBQT-12 | Native fit fixtures and generated-FCS 4PL/insufficient-input e2e evidence exist. | I production fit view plus declared units/pairing/weighting/model and suspect-fit evidence. | [inference_test.cpp](../../tests/analysis/inference_test.cpp) |
| I11 | MIBQT-11 | QC thresholds/exclusions remain an open implementation slice. | Native versioned thresholds, reversible exclusions and retained old-verdict lineage. | [2026-11-02-fold-in-mechanism-realignment.md](../../docs/exec-plans/active/2026-11-02-fold-in-mechanism-realignment.md) |
| I12 | MIBQT-16 | Workspace save/restart is recorded; finding/draft integration has branch-specific documentation. | Review immutable import/export and finding-to-recipe-authority linkage at this exact head. | [PRODUCTION-INTEGRATION.md](../../docs/design/ique-workflow/PRODUCTION-INTEGRATION.md) |
| F01 | MIBQT-07,16 | Scale test includes streaming fingerprint over 64 MiB and an independent hash. | Verified relink, stable IDs and acquired-name/label/unit semantics; attach current-head results. | [review_population_scale_test.cpp](../../tests/review/review_population_scale_test.cpp) |
| F02 | MIBQT-16 | Native graph/source analysis test candidate; complete QC invalidation proof not reviewed. | Persist good/bad/unknown with reasons; changed membership invalidates every dependent result. | [graph_test.cpp](../../tests/analysis/graph_test.cpp) |
| F03 | MIBQT-13 | Compensation/Logicle work remains open in the realignment plan. | Persist order/scope and native evaluation; unsupported-channel refusal with numeric oracle. | [2026-11-02-fold-in-mechanism-realignment.md](../../docs/exec-plans/active/2026-11-02-fold-in-mechanism-realignment.md) |
| F04 | MIBQT-07,09,13 | 300k-event synthetic scale test and native DAG exist; e2e records smaller synthetic populations. | Private FlowJo 229585/190705 fixture; representative real MIB 200k+ recording with memory bound; drawn gate editing and revision proof. | [review_population_scale_test.cpp](../../tests/review/review_population_scale_test.cpp) |
| F05 | MIBQT-07,16 | E2e records histogram sum = measured population n (3900). | Full plot/decimation and revision-binding review plus exact-head test evidence. | [columnar_test.cpp](../../tests/analysis/columnar_test.cpp) |
| F06 | MIBQT-16 | Gallery pixels observed in recorded synthetic e2e. | Lossless IDs above 2^53 and expired-reason round trip through the production resolver. | [evidence-gallery-app-parity.mjs](../../tools/flowjo_acceptance/evidence-gallery-app-parity.mjs) |
| F07 | MIBQT-16 | Supervised embedding worker/test sources exist. | Reproducibility with pinned seed/runtime/revisions/subsampling and event-keyed coordinates. | [worker.rs](../../crates/mib-analysis-worker/tests/worker.rs) |
| F08 | MIBQT-07,16 | Native statistics tests and e2e n/mean/median/CV/percentile output exist. | Independent oracle coverage for every estimator, units, approximation labels and null denominators. | [statistics_test.cpp](../../tests/analysis/statistics_test.cpp) |
| F09 | MIBQT-07,12 | Native inference/fit fixtures; recorded 4-vs-4 units, 21-unit 4PL and insufficient cases. | Complete model/pairing/weighting/compatibility/suspect-fit review and production view proof. | [inference_test.cpp](../../tests/analysis/inference_test.cpp) |
| F10 | MIBQT-16 | Native report/host tests exist; prior branch reports are not fresh integrated-head proof. | Immutable ordering, cancellation/retry/name-collision evidence with prior artifacts preserved. | [analysis_host_test.cpp](../../tests/analysis/analysis_host_test.cpp) |
| F11 | MIBQT-16 | Native population/overlay test candidate exists. | Normalization-on/off equality of membership and statistics through the production command. | [population_test.cpp](../../tests/analysis/population_test.cpp) |
| F12 | MIBQT-07,16 | E2e records workspace history/pinned evaluation surviving restart; workspace scale test exists. | Document precedence and demonstrate incompatible changes force recomputation. | [review_workspace_scale_test.cpp](../../tests/review/review_workspace_scale_test.cpp) |
| F13 | MIBQT-08,16 | Live evidence test exists. | Recorder-unaffected stress, finalized-input binding and representative rig run. | [live_evidence_test.cpp](../../tests/processing/live_evidence_test.cpp) |
| F14 | MIBQT-06,16 | Finding linkage and native recipes exist; complete cross-authority flow needs review. | Prove next-run draft goes only through I02 with no device command. | [PRODUCTION-INTEGRATION.md](../../docs/design/ique-workflow/PRODUCTION-INTEGRATION.md) |
| F15 | MIBQT-07,09,13 | Native FCS reader/hardening tests; generated FCS import used in integrated e2e. | Private FlowJo fixture through native evaluator; FCS/GatingML export declarations and unsupported-construct handling. | [fcs_reader_test.cpp](../../tests/analysis/fcs_reader_test.cpp) |
| F16 | MIBQT-07,16 | Worker crash/cancel/restart test candidates and native scale tests exist. | Exact-head timeout/crash/cancel/restart and stale-cache matrix with full-size memory bound. | [worker.rs](../../crates/mib-analysis-worker/tests/worker.rs) |

## Integration boundaries and next work

- PR #816 now contains the combined E/I/F integration, not just goals/ADR/guard
  documentation. The tracker description and the dated plan checklist should not
  be read as a fresh inventory of that PR. No merge is performed by this review.
- #827 tours and #809 discovery guards are explicitly excluded by #816's current
  description. Review their exact diffs before crediting wiring/guard evidence.
- MIBQT-14's separately recorded first-frame branch is not counted as integrated.
- Earlier E Windows capture reports concern another branch and zero object rows;
  they cannot certify E01–E06 on this head or physical event-gallery acceptance.
- The private FlowJo fixture is a specific F04/F15 verification dependency, not
  a blocker for unrelated implementation. Do not infer its availability from a path.
- Immediate independent implementation candidates remain I03 estimates, I07 named
  metrics and I11 versioned QC. I08 and I09/I10 must consume the existing native F
  authority rather than fork scientific calculations into TypeScript.

## Updating this index

For each changed goal, record the tested commit, exact command, outcome, evidence
class/data scale and retained log location. State authority, contract, production
wiring and remaining proof explicitly. Do not replace an unavailable hardware or
reference-data result with a mock pass. New tests do not retroactively certify an
older branch, and a green PR does not establish scientific/platform acceptance.
Update the corresponding tracker action and Log when authorized, preserving owner
and decision holds. Develop promotion remains MIBQT-18's explicit owner decision.
