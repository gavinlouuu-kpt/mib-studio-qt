## 2026-10-22 — Review info names a regenerated file's E-LUT; local profiles identify the method (#783)

Two backend gaps from the #782 click-through.

**Review ABI 5.** `fetch_review_info` (and `review_probe_runs`) read the identity's
`emodulus_lut` from the run snapshot only, so a regenerated file (no snapshot, but
`/experiment_info @emodulus_lut_json` since #764) showed "E-LUT Not recorded". `RunIdentity` now
falls back to that attribute (the snapshot record wins, as in `recording::readFileEModulusLut`) and
adds `emodulus_lut_provenance`: `snapshot` | `recorded` | `host` (contract
`run_identity.emodulus_lut_provenances`, `REVIEW_EMODULUS_LUT_PROVENANCES`), `null` exactly when
`emodulus_lut` is. Additive; review ABI 4 → 5 (free on develop and on every open PR). Touched:
`ReviewTypes.h`, `ReviewSession::readRunIdentity`, `review_shim.cpp` (+ fixture
`review_fixture_write_regenerated`), `review_bridge.rs`, `review.rs`, `reviewBridge.ts`
(`decodeReviewEModulusLutProvenance`), the generator (renders and checks the strings against the
C++). No UI change: the workspace branch wires the words and B3d.

**Local profiles.** A run under a local profile (`ProfileStore`) recorded no method identity: the
calibration record's method block had no id or name, and the snapshot's `profile_id` was empty.
`attachLocalProfile` reads the `profile_selection` ProfileStore writes into the applied config text
into `MethodProvenance::localProfile{Name,Path,Revision,Id}` (local methods only). The run snapshot
v3 (no new version) carries it as `method.local_profile {name, path, revision, profile_id}` and as
`profile_id` when the client declares none; the calibration record uses its existing members
(`method_id` = profile_id, `revision_id` = revision, `display_name` = name, `config_sha256`), so no
bridge reply shape changed (bridge ABI stays 43). The `method.revision` gate's detail names the
profile. Legacy files and stored records read as before.

Tests: `review.run_identity`, `backend.method_provenance`, `e2e.calibration_profile_apply`,
`review_bridge.rs` `regenerated_file_identity_names_its_elut_record`, `review.rs`,
`reviewIdentity.test.ts`. See [[services/ReviewSession]], [[frontend/YofoReview]],
[[architecture/Rust-Bridge]], [[services/CalibrationRecords]], [[data-model/HDF5-Storage]].
