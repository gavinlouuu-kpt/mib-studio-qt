# Tauri catch-up integration

Status: active

## Goal

Bring `dev/react-tauri` onto current develop and complete independent operator
and backend integration slices in parallel without removing the Qt shell.
The September 23 readiness assessment records the starting gaps; this is not
an assertion of production cutover readiness or hardware acceptance.

## Acceptance and progress

- [x] Fast-forward local migration branch to develop `2fe0282` (252 commits).
- [ ] Existing camera-script/reset APIs exposed as real operator controls.
- [ ] Existing pump/autofocus APIs exposed with commissioning/lifecycle guards.
- [ ] Checked Qt-free processing-config persistence contract and Tauri adapter.
- [ ] Reuse HdfExportService for cancellable export/status through the bridge.
- [ ] Integrate and execute frontend, contract, native backend/bridge checks.
- [ ] Publish the updated branch with exact tested and unverified scope.

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
