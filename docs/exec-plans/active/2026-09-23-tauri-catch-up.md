# Tauri catch-up integration

Status: completed (scoped catch-up batch; migration cutover remains open)

## Goal

Bring `dev/react-tauri` onto current develop and complete independent operator
and backend integration slices in parallel without removing the Qt shell.
The September 23 readiness assessment records the starting gaps; this is not
an assertion of production cutover readiness or hardware acceptance.

## Acceptance and progress

- [x] Fast-forward local migration branch to develop `2fe0282` (252 commits).
- [x] Existing camera-script/reset APIs exposed as real operator controls.
- [x] Existing pump/autofocus APIs exposed with commissioning/lifecycle guards.
- [x] Checked Qt-free processing-config persistence contract and Tauri adapter.
- [x] Reuse HdfExportService for cancellable export/status through the bridge.
- [x] Integrate and execute frontend, contract, native backend/bridge checks.
- [x] Publish the updated branch with exact tested and unverified scope.

## Decision log

- 2026-09-23: user explicitly requested parallel work. Use separate worktrees
  for hardware controls, config contract and export bridge. Parent owns App
  integration and native verification; shared bridge edits are reconciled once.
- 2026-09-23: preserve the dirty Qt/FPGA checkout. No hardware actuation during
  implementation. Existing services are the implementation source; no parallel
  exporter or experiment coordinator is introduced.
- 2026-09-23: camera script picker applies existing EGrabber script files only;
  it does not pretend to provide script text editing or MindVision script support.

## Remaining cutover scope

Authoritative frame identities/clock domains, general operation resnapshot,
profiles, chart/overlay/reanalysis parity, current hardware endpoint coverage,
real native workflow/Qt comparison, performance, installers/updater and Windows
acceptance remain tracked by #372/#246 and the readiness assessment. Passing a
build or process-alive smoke is not a native end-to-end workflow pass.


## Executed integration checks

156 frontend DOM/unit tests and TypeScript/Vite production build pass. Five targeted native tests
pass: checked configuration, facade boundary, experiment coordinator, shared
HDF5 exporter and export facade. ABI generation, docs and screenshot checks
pass. Native desktop compilation and all 16 desktop Rust tests pass locally
and in Desktop CI at `9ec8e98`; the CI Xvfb process-alive smoke also passes.
All 17 shared Rust/C++ bridge tests pass in CI at the same commit.
No production-cutover or real-hardware result is claimed.

Source fixes discovered during verification: export test registration before
runner finalization; absence-aware metadata handling for valid-only recordings;
unknown-key-preserving changed patches; runtime provenance includes only the
processing fields actually applied, not unrelated sections from the saved file.


Native targeted ThreadSanitizer checks: 5/5 passed with `setarch x86_64 -R`
(per-process ASLR disabled; normal launch hit TSan unexpected-memory-mapping on
this host). Shared bridge CI passed at `6239d10`. Monitoring chart placeholders
were replaced with bounded raw-unit plots; packaging/full Qt comparison remains
outside this batch.

Published as draft [PR #450](https://github.com/gavinlouuu-kpt/mib-studio-qt/pull/450)
on `dev/react-tauri`. Broader backend/sanitizer/platform PR jobs were still
running when this verification record was finalized; their completion is not
implied by the targeted results above.
