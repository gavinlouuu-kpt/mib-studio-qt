## 2026-10-08 — The applied method survives a restart: one startup pointer (#398 M2c)

Gavin chose one startup pointer over a separate method restore, so local profiles and
central methods cannot both claim to be in effect after a restart.

`app/StartupConfiguration` keeps `<dataDir>/startup_configuration.json`, the last applied
local profile (folder, name, revision) or central revision (id + config sha256), recorded
by both apply paths. `restore_startup_configuration` (bridge ABI 32) re-applies it at
startup through the shared validator:
- a profile from its recorded folder;
- a central revision from the registry cache, reopened offline, which must still match the
  recorded sha256;
- with no pointer, a legacy folder selection.

Rules:
- A failed restore (missing, revoked or changed revision; changed profile) applies nothing
  and returns a notice that the instrument runs on its default settings. The profiles panel
  shows it. The pointer stays, so the notice repeats until something else is applied.
- An ROI with no frame yet is pending (applied on the first frame), never a failure.
- The profiles hook now runs the restore even without a profiles folder.

Guards:
- `profiles.startup_configuration`: a real restart, a changed sha, last-wins both ways,
  revoked and missing revisions with notices, a pending ROI on restore, the legacy selection;
- `contract.rs`;
- `profiles.test.tsx`.

See [[architecture/Desktop-Shell]] and [[architecture/Rust-Bridge]].
