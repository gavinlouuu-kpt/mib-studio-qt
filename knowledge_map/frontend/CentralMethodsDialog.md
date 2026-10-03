# Central Methods Dialog

> Settings UI for the central profile registry (#398 M1): sign in, refresh,
> and list the central method revisions cached for the current user, each with
> its own central state. Read-only toward the instrument.

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
prefix (full hash in the tooltip) and author. A note states that central
state is not local validation and that listing does not select or apply.

## Gotchas

- The dialog never calls the network or the cache: it enqueues worker
  commands and renders `snapshot()` from a 200 ms timer that runs only while
  visible, re-rendering only when the snapshot generation or busy flag
  changes. The GUI thread stays responsive while a registry request hangs.
- Unconfigured (no `MIB_PROFILE_REGISTRY_URL` / `_PUBLISHABLE_KEY`, or no
  shell transport): the status says so and every control is disabled.
- No method selection, Apply, update-available badge or context-bar entry yet
  (later #398 milestones).

Test: `frontend.central_methods` (offscreen dialog over the real worker and a
fake Supabase from `tests/support/fake_supabase.h`).
