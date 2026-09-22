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

## UI TSan startup isolation verified (2026-09-22)

The prior desktop input-method startup path produced Qt/DBus TSan reports.
Changing accessibility/GLib settings did not remove them. Selecting the local
Qt compose input method (`QT_IM_MODULE=compose`) while retaining the offscreen
platform lets the unchanged real-tab test and backend stress test pass under
TSan with `halt_on_error=1` and **no suppressions**. CTest now sets that input
method explicitly for this hardware-free test. No product runtime setting,
callback assertion, stress iteration, or sanitizer check is disabled.

Reproduction: `TSAN_OPTIONS=halt_on_error=1 setarch x86_64 -R ctest
--test-dir build/callback-tsan -R '^(frontend.nanopositioner_callback|backend.autofocus_callback)$'
--output-on-failure`. The process-local ASLR workaround remains host-specific.
This validates callback lifetime and threading, not desktop input-method/DBus
integration or physical hardware. The original UI TSan gate is now unblocked.

The first TSan build also picked Homebrew fmt with system spdlog; using the
system-only package exclusions from the Linux preset corrected compilation.
Local build logs live under each dedicated `build/callback-*` directory.

The unidentified and invalid-minidump tasks in #405 remain open: matching
symbols and evidence locating dump corruption are still required. Historical
recovered-dump notifications are not new crash incidents.
