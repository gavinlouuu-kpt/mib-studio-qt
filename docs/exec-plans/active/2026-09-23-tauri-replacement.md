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
- [x] Authoritative frame identity and retained operation/state reconciliation.
- [x] Native webview mock configure/start/finalize/reopen/export acceptance.
- [x] Same-backend Qt/Tauri fixture comparison and lifecycle failure checks.
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

Published through `e107689` on `dev/react-tauri` / draft PR450; subsequent review
fixes continue as small commits. Keep Qt until remaining software/release gates
and the separately deferred hardware acceptance are satisfied.

- 258 frontend tests and production build passed at the latest integrated batch.
- 25 Rust/Tauri native tests, 12 targeted backend tests and 21 targeted TSan
  tests passed (including source identity, config faults, export, profile/core,
  startup policy, serial fake bus and capture/FrameStore stress).
- Full SDK-free non-network/non-hardware CTest sweep: 114 passed and one optional
  exporter soak skipped. Missing NumPy was repaired in an isolated build venv.
- `frontend.review_parity` passes: Qt shared chart calibrated ranges, exact
  histogram/density conservation and isoelastic curves match the bridge, and
  all five saved overlay modes match every decoded RGB pixel. Scope is saved
  scientific rendering, not physical acquisition or all interactive Qt paths.
- Native production WebKit `native-folder-preserve` passes actual GTK dialogs, mock capture,
  refused active-run Exit, complete webview reload preserving the same run,
  finalization/conserved nonzero persistence, HDF reopen, shared export, mask regeneration with unchanged source digest, regenerated
  output reopen/core identity, close review and successful idle Exit. Export asserts the exact chosen destination.
  No backend mocks. Hosted picker and smoke-process cleanup failures were reproduced
  and fixed. Desktop CI passed at `e107689` (run 35826548498), including native
  workflow and extracted-package replay. Backend and Bridge CI passed too.
- Linux debug and optimized release `.deb` built with derived native dependencies.
  The optimized release passed capture/reload/finalize/reopen/export/reanalysis/
  regenerated-output-reopen/exit outside the repository and loaded bundled LUT.
  `ldd` found no missing libraries or Qt runtime dependencies. CI repeats extracted
  package acceptance. Windows SDK-free,
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
- Camera script/JSON editing, checked Save As and bundled defaults are integrated.
- Live monitoring retains analysis-time calibration; stale rows are never rescaled.
- Fault recovery requires the exact displayed run/fault revision and preserves
  failed output/accounting.
- Application installers require canonical platform-specific Tauri manifests,
  streamed SHA256 verification, expiry/refetch checks and explicit idle-state launch.
  Version comparisons use the installed Tauri package, not the independent native
  core compatibility version. Staging is capped at 4 GiB; explicit guarded cleanup
  preserves unrelated files and reports locked packages without launching anything.

## Remaining completion work

- Windows candidate result (MSVC byte-exact resource regression passed; build
  remains in progress). Combined local checks and optimized
  Linux package replay pass. Verified explicit installer launch is implemented and fixture-tested;
  the platform-specific Tauri manifest publisher is now implemented and
  regression-tested in dry-run mode, but no feed has been published and real
  installer acceptance remains a release gate.
  Existing Qt installer feeds are rejected. No release/feed was published.
- Hardware suite, SDK-equipped deployment and physical timing remain deferred by
  Gavin; mock, fake-serial and SDK-free tests do not establish those claims.
