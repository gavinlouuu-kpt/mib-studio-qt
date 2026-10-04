# Central Methods Dialog

> Settings UI for the central profile registry (#398 M1/M2b): sign in,
> refresh, list the cached central method revisions with their central state
> and local validation on this instrument, Apply one (exact config.json,
> backed up) and record a local validation from a test run.

**Source:** `src/frontend/dialogs/CentralMethodsDialog.cpp`,
`include/frontend/dialogs/CentralMethodsDialog.h`
**Related:** [[MainWindow]], [[Dialogs]], [[../services/ProfileRegistryService]],
[[../architecture/Threading-Model]]

## User flow

**Settings → Central Methods…** opens a modal dialog over
`AppBackend::profileRegistry()`. It shows registry connectivity (online,
offline, sign-in required, access denied, not configured), the account
(signed in / cached offline for the last user / not signed in), the last
refresh and last action, and warnings (cache errors, revisions hidden for
failing integrity, registry revisions rejected during sync). Signed out, an
email + password row signs in; the password field is cleared on submit and
only the worker's queued command holds it. Signed in, **Refresh** runs a
bounded refresh of every member project; **Cancel** aborts the running
request; **Sign out** closes the user's cache. Opening the dialog while
signed in requests a refresh (an explicit sync point; there is no background
polling of the registry).

The table lists method, `r<number>`, project display name, central state
(Published, Superseded, Archived, Approved (not published), Submitted,
Rejected, **REVOKED - do not use** in bold red), a 12-character content-hash
prefix (full hash in the tooltip), author, and **On this instrument**:
`APPLIED` when the applied config.json is exactly that revision's, the local
validation for this instrument UUID + current method context (validated by
… / validation FAILED / not validated; `backend::app::localValidationFor`),
and `files ready` when materialized. The instrument label shows
`MIB_INSTRUMENT_NAME` and the UUID.

M2b actions on the selected row (constructor takes `CentralMethodsHooks`;
each missing hook disables its action with a tooltip):

- **Apply…** — published/superseded only. Materializes first when needed
  (waits for that job in `poll()`), then `planMethodApply()` (refuses
  tampered files) and a confirmation listing the changed config.json keys, the
  backup and the camera-script path (not applied). On Yes,
  `hooks.applyConfig` = `AppConfigWatcher::applyMethodDocument` writes the exact
  bytes after a `config.json.bak-<UTC stamp>` backup and reloads, so the
  coordinator's `method.revision` gate recognises the revision.
- **Mark validated…** / **Record failed run…** — signed in, instrument known,
  published/superseded. Picks an `.h5` and calls
  `AppBackend::requestMethodValidation`; a refusal (wrong revision, instrument
  or context, local-method run, unreadable file) is shown in the notice line.

[[MainWindow]] builds the hooks from `AppBackend` and the Preview page's
`AppConfigWatcher`.

## Gotchas

- The dialog never calls the network or the cache: it enqueues worker
  commands and renders `snapshot()` from a 200 ms timer that runs only while
  visible, re-rendering only when the snapshot generation or busy flag
  changes. The GUI thread stays responsive while a registry request hangs.
- Unconfigured (no `MIB_PROFILE_REGISTRY_URL` / `_PUBLISHABLE_KEY`, or no
  shell transport): the status says so and every control is disabled.
- Besides the snapshot generation, `poll()` re-renders when the applied
  config.json text or the method context hash changes (neither bumps the
  registry generation).
- No update-available badge or context-bar entry yet (later #398 milestones).

Test: `frontend.central_methods` (offscreen dialog over the real worker and a
fake Supabase from `tests/support/fake_supabase.h`; M2b: apply with declined
and accepted confirmation, exact bytes, backup, APPLIED marker, revoked row
disabled, validation refusal shown) and `frontend.config_apply`
(`applyMethodDocument` on a real config.json).
