# Tauri checked processing configuration transactions

The Qt-free `ProcessingConfigTransaction` seam is exposed by
`BackendFacade::fetchConfigDocument(path)` and
`BackendFacade::applyConfigDocument(path, baselineRevision, patchJson)`.

- Baselines are SHA256 of the raw file bytes, not timestamps. Missing/stale
  baselines fail as conflicts. Reload explicitly; there is no force-overwrite.
- The bounded patch schema is exactly `{"image_processing": {...}}`.
  Existing `ProcessingConfigJson` field mapping is reused. Unknown document
  keys survive; unknown patch keys, null deletion, wrong types, invalid
  merged enabled ranges and invalid kernel sizes are rejected.
- Save uses an exclusive same-directory temporary file, checked write and
  flush, file sync, atomic replacement, and parent directory sync on POSIX.
  No runtime changes occur on pre-commit persistence failure.
- Outcomes distinguish `saved`, `applied`, `verified`, and `conflict`, with
  the document revision and an error. A directory-sync failure can report
  saved=true/applied=false; callers must not treat this as full success.
- `ExperimentCoordinator::withIdleConfiguration` serializes transactions
  with Start using the existing lifecycle mutex. Starting/Active/Stopping/
  Failed states reject changes. The callback must not reenter the coordinator.
- Like the Qt `ConfigDocumentStore`, the baseline check is not an
  interprocess compare-and-swap. External writers can still race a rename.

Regression coverage: precision/unknown-key roundtrip, stale baseline,
malformed/null/unsupported patches, filesystem replacement fault, and eight
concurrent requests sharing one baseline (only one can save).

This is a bounded G4 seam, **not full G4 closure**: the Qt watcher has not yet
been converted to this client; camera/ROI/calibration/realtime configuration
and cross-client runtime setters are outside this transaction. Native Windows
filesystem behavior requires CI validation.

Related: [[architecture/Rust-Bridge]], [[architecture/ExperimentCoordinator]].

Document reads are bounded to 4 MiB and patches to 64 KiB before JSON parsing.
The merged persisted image-processing section supplies all known runtime
values it contains, including unpatched fields; absent values retain runtime
state. A disk/runtime divergence regression protects that authority rule.
