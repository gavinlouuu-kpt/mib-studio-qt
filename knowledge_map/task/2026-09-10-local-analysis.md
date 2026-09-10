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
- `cargo check --manifest-path desktop/src-tauri/Cargo.toml --features analysis-only`
  passed with provisioned Linux GTK/WebKit dependencies. This is compilation,
  not a real-window or clean-installer acceptance test.

Windows clean-machine/mapped-drive/UNC, real NAS failures, standalone installer,
owned helper, toolkit parity, reanalysis, derivatives and optional sync are not
claimed. POSIX open-handle rename is a local source-loss simulation, not NAS
qualification. Observed size/mtime identity is not content-verified identity.
The documented 0.1.0 toolkit release manifest, latest manifest and PEP 503
package index all returned 404; production artifact
pinning remains blocked until an actual released wheel/digest is available.
# Registry and helper continuation

Investigated the registry 404: Toolkit has no tags/releases/release workflow runs;
MiB's manifest on the same domain returns 200. Toolkit's R2 publisher also omitted
the exact pip directory keys. Fixed in
https://github.com/gavinlouuu-kpt/Biowork-toolkit/pull/2 with regression failure on
the original source, 57 passing tests, clean Ruff and successful wheel/sdist build.
No release or credential changes performed.

Added `desktop/analysis/helper.py` and its executable private-pipe harness. Source
pin remains Toolkit `388924e5c9d95e0691b969be6238f3e94db817d4`, version 0.1.0;
the registry patch has no scientific package changes. Validation uses an installed
locally built wheel, with isolated Python (`-I`), not an import mock. Protocol 0.1
has bounded framing, monotonic request IDs, operation/generation identity, strict
handshake, generation invalidation and Toolkit histogram/KDE page parity. Tests
cover bad envelopes/frames, budgets, replay, nonfinite/overflow inputs, maximum
pages, repeated exits and parent-forced termination/reaping. There are no new
application threads or shared-state changes.

Executed continuation gates: 8 subprocess integration tests passed on Linux
Python 3.12 (including 10 repeated maximum-page runs); Ruff and `check_docs.py`
passed. Windows and production supervisor behavior were not exercised.

The helper is development-only and does not enable any desktop capability. It
does not claim full-recording analysis, cancellation during computation, signed
bundle validation, crash recovery, or a clean-machine installer. Those production
milestones remain open; absence of a registry wheel is not a reason to halt their
source-based development.
