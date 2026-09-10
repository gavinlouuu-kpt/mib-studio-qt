# #399 desktop local analysis foundation

Status: native foundation implemented; overall issue remains open.

See [execution plan](../../docs/exec-plans/active/2026-09-10-local-analysis.md)
for the inventory, helper protocol proposal, dependency pin and milestone gates.

## Delivered

- Analysis-only native startup and command permissions, exposed through bridge
  ABI 14 and Tauri startup/feature selection.
- Finalized-file admission, persisted completion/accounting and observed source
  revision checks; no raw source mutation.
- Bounded measurement hyperslabs (4,096 rows), shape-only open, sparse image
  budget (32 MiB), no full-file metadata cache or background-image read on open.
- Shared React Review starts directly in analysis mode, uses original raw HDF5
  rows rather than the live ring, and marks unavailable raw metrics honestly.

## Evidence

- `review_metadata_budget_test` fails against the original linked backend:
  UINT64_MAX page request was accepted. Same test passes with the fix.
- Native `backend.analysis_workspace`, `backend.review_metadata_budget`,
  `backend.facade_boundary`, `recording.hdf5_resilience`: 4/4 passed.
- Analysis fixture covers a 100,000,000-row sparse extent, exact metadata and
  pixel identities, processed metadata parity, oversized/overflow/malformed
  requests, repeated handle counts, source bytes unchanged, and source faults.
- Rust/cxx bridge: 17/17 contract tests passed, including analysis-only denial.
- Frontend: strict TypeScript/Vite build and 117/117 Vitest tests passed.
- ThreadSanitizer build of `backend.analysis_workspace`: passed, no reported races.
- Documentation, screenshot-index and generated bridge-contract checks passed.

Windows clean-machine/mapped-drive/UNC, real NAS failures, standalone installer,
owned helper, toolkit parity, reanalysis, derivatives and optional sync are not
claimed. POSIX open-handle rename is a local source-loss simulation, not NAS
qualification. Observed size/mtime identity is not content-verified identity.
The documented 0.1.0 toolkit release manifest returned 404; production artifact
pinning remains blocked until an actual released wheel/digest is available.
