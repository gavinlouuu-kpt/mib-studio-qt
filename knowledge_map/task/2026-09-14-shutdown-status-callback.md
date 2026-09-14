# Shutdown status callback lifetime — #405

## Scope and evidence

Refs GitHub #405 (not a closing fix for its diagnostic slices). Sentry group
136 has four beta v1.1.1 events; latest remains 2026-09-10 05:55:11 UTC.
The supplied stack runs from AppBackend/AutofocusService destruction through
the registered NanopositionerTab callback into a deleted UI. Current remote
main `ad82cf6` still registered a raw `this` capture without lifetime handling.
No newly observed crash or Sentry status change is claimed.

## Change

- [[../frontend/NanopositionerTab]] owns a shared admission gate invalidated
  before UI deletion. Callbacks retain the gate only; admitted messages are
  context-bound queued Qt invocations. Pending messages die with the receiver.
- [[../services/AutofocusService]] snapshots callbacks under the registration
  mutex, invokes outside it, and documents that snapshots can outlive replacement.
- Real Qt tab test uses only the unsupported-platform SDK stub. No hardware
  connection, physical shutdown voltage, release, or deployment was exercised.

## Regression and verification

- Before the UI fix: the real tab accepted a status, was deleted, and the same
  callback fired again through the hardware-disabled stub. Process exited 139
  (segmentation fault). The fixed regression passes.
- Backend stress: 10,000 callback emissions race with 10,000 replacements.
  With the original stub restored temporarily, TSan exited 66 and reported
  `setStatusCallback` writing `std::function` versus `connect` reading it.
  Restoring the fix makes that same TSan test pass.
- UI stress covers 50 destroy/recreate cycles, queued delivery only after the
  GUI event loop runs, pending-event cancellation, late emissions, concurrent
  replacement and callback self-unregistration. A watchdog bounds all joins.
- Release: 2/2 pass; ASan/UBSan: 2/2 pass (including leak detection defaults).
  Checks use CTest names `frontend.nanopositioner_callback`
  and `backend.autofocus_callback` in hardware-disabled full Qt builds.
- Documentation/vault links, screenshot manifest synchronization, and diff
  whitespace checks pass.
- TSan backend test passes with the existing suppression file and process-local
  `setarch x86_64 -R`; no new suppressions or global ASLR changes.

## Outstanding verification blocker

The full Qt TSan test cannot reach its test body on this host: system Qt 6.4.2
reports allocation/deletion synchronization in the `QDBusConnection` thread
(`QCoreApplicationPrivate::sendPostedEvents`) during QApplication startup.
The normal TSan launch first failed with `unexpected memory mapping`; the
process-local ASLR workaround exposed the Qt startup report. Do not count
this as a full UI TSan pass. An instrumented Qt runtime or evidence-backed
startup isolation is needed before completing that requirement. Keep the PR
in draft until this gate is satisfied; do not blindly retry the same setup.

The first TSan build also picked Homebrew fmt with system spdlog; using the
system-only package exclusions from the Linux preset corrected compilation.
Local build logs live under each dedicated `build/callback-*` directory.

The unidentified and invalid-minidump tasks in #405 remain open: matching
symbols and evidence locating dump corruption are still required. Historical
recovered-dump notifications are not new crash incidents.
