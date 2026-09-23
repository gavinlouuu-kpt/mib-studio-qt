# Tauri software replacement completion

Status: active

## Authorization and scope

Gavin requested review, gap closure, continued implementation and pushes to
`dev/react-tauri` until it can replace Qt. Continue the previously authorized
parallel work. Hardware suite execution is explicitly deferred; do not actuate
hardware or equate mock/SDK checks with hardware acceptance. Preserve Qt and
unrelated worktrees during implementation.

## Completion gates

- [x] Review the catch-up code for correctness, not only feature presence.
- [x] Typed hardware endpoint, pulse generator and startup discovery controls.
- [x] Shared local profiles and configuration activation/persistence parity.
- [x] Review charts, batch metrics and reanalysis/mask regeneration.
- [x] Preview pause/scrub, buffer save, overlays and background calibration.
- [ ] Authoritative frame identity and retained operation/state reconciliation.
- [x] Native webview mock configure/start/finalize/reopen/export acceptance.
- [ ] Same-backend Qt/Tauri fixture comparison and lifecycle failure checks.
- [ ] Packaging, update/release path and resource/performance verification.
- [ ] Publish software result with explicit deferred hardware/platform evidence.

## Ownership

Independent worktrees own hardware, review/reanalysis and profile/config slices.
Integration owner owns preview, identity/recovery, cross-cutting review and
native acceptance. Shared bridge changes are reconciled on integration branch.
Reuse existing backend services and Qt scientific implementations; no duplicate
analysis pipeline in TypeScript/Rust.

## Review findings

- Latest-frame facade queried an index then fetched a newer latest frame: under
  concurrent capture this can pair old identity with new pixels. Add concurrent
  identity regression and fetch the exact committed index.


## Integrated software evidence (2026-09-23)

Published through `bcd3c53` on `dev/react-tauri` / draft PR450; subsequent review
fixes continue as small commits. Keep Qt until remaining software/release gates
and the separately deferred hardware acceptance are satisfied.

- 231 frontend tests and production build passed at the latest integrated batch.
- 18 Rust/Tauri native tests, 12 targeted backend tests and 16 targeted TSan
  tests passed (including source identity, config faults, export, profile/core,
  startup policy, serial fake bus and capture/FrameStore stress).
- Full SDK-free non-network/non-hardware CTest sweep: 113 passed, one optional
  exporter soak skipped, one Python conformance-input test could not import the
  host's missing NumPy. Repairing the local verification environment; do not
  describe the sweep as entirely green until that script passes.
- `frontend.review_parity` passes: Qt shared chart calibrated ranges, exact
  histogram/density conservation and isoelastic curves match the bridge, and
  all five saved overlay modes match every decoded RGB pixel. Scope is saved
  scientific rendering, not physical acquisition or all interactive Qt paths.
- Native production WebKit workflow9 passes actual GTK dialogs, mock capture,
  refused active-run Exit, complete webview reload preserving the same run,
  finalization/conserved nonzero persistence, HDF reopen, shared export, close
  review and successful idle Exit. No backend mocks.
- Linux debug `.deb` built with derived native dependencies; extracted package
  performed capture/finalize/reopen/export outside the repository and loaded
  bundled LUT. CI now repeats extracted-package acceptance. Windows SDK-free,
  unsigned portable candidate build/smoke added; execution pending hosted CI.

## Correctness fixes discovered during integration

- Exact indexed frame fetch replaces index/latest-pixels race. MIBF v2 carries
  capture-session and FrameStore-resize epochs; processed previews retain their
  exact source, recipe and capture epoch separately (host-only ABI metadata).
- Raw review reads saved frames, never the live ring. Source lifecycle and metrics
  responses are generation guarded; export pins its source before the picker.
- Readiness/start is backend-authoritative; processing consumer is started by
  the shared experiment coordinator. Pending background calibration blocks Start.
- Config validates the whole candidate before mutation and checks revision;
  dirty drafts survive refresh and stale drafts cannot be applied silently.
- Reload reconciles capture/run/review/jobs before publishing readiness and never
  reapplies profiles/cores over a retained native session. Writer handles are not
  interpreted as saved review sources. Close protects pending/active work.
- Resource lookup separates read-only installed assets from writable user data.

## Remaining completion work

- Camera script text editing/save and complete MindVision JSON workflow parity.
- Exact per-row calibration provenance for calibrated live monitoring (never
  relabel retained raw areas with a newly selected calibration).
- Explicit generation-checked fault acknowledgement and recovery controls.
- Final combined checks, Linux package replay, hosted CI and Windows candidate
  result. Signed automatic application updates require a Tauri-specific feed;
  existing Qt installer feed must not be installed by this shell.
- Hardware suite, SDK-equipped deployment and physical timing remain deferred by
  Gavin; mock, fake-serial and SDK-free tests do not establish those claims.
