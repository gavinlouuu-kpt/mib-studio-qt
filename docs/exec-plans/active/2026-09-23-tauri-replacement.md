# Tauri software replacement completion

Status: active

## Authorization and scope

Gavin requested review, gap closure, continued implementation and pushes to
`dev/react-tauri` until it can replace Qt. Continue the previously authorized
parallel work. Hardware suite execution is explicitly deferred; do not actuate
hardware or equate mock/SDK checks with hardware acceptance. Preserve Qt and
unrelated worktrees during implementation.

## Completion gates

- [ ] Review the catch-up code for correctness, not only feature presence.
- [ ] Typed hardware endpoint, pulse generator and startup discovery controls.
- [ ] Shared local profiles and configuration activation/persistence parity.
- [ ] Review charts, batch metrics and reanalysis/mask regeneration.
- [ ] Preview pause/scrub, buffer save, overlays and background calibration.
- [ ] Authoritative frame identity and retained operation/state reconciliation.
- [ ] Native webview mock configure/start/finalize/reopen/export acceptance.
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
