## 2026-11-06 — An installed app keeps its state in %LOCALAPPDATA% (rig check F1, F3, F4)

The pilot rig check (`v1-02-rig-check`, beta 98c38104) found that a Program Files install kept its
state in `C:\Program Files\MIB Studio Qt\data`, which a normal user cannot write: every launch
logged `instrument identity could not be written` and ran as `instrument id <unknown>`, and the
calibration records, SQLite database and profile cache went to the same folder. Changes:

- **One data-dir rule** (`include/backend/app/DataDirectory.h`, Qt-free): `MIB_DATA_DIR`, else
  `%LOCALAPPDATA%\MIB_Studio_Qt\data` on Windows when the exe is under Program Files (by path, so
  elevated and normal launches agree) or `<exe dir>/data` is not writable, else `<exe dir>/data`
  (dev, portable, Linux: unchanged). `main.cpp` resolves it once; the log dir
  (`%LOCALAPPDATA%\MIB_Studio_Qt\logs`, unchanged) and crash dir come from the same layout, and
  `AppBackend` uses the same `logDirFor`. The four "program files" checks in `main.cpp` and
  `AppBackend.cpp` are gone. The shell now sets the resource root, since the bundled LUT is no
  longer under the data dir's parent. See "Data directory" in [[architecture/AppBackend]].
- **One-time migration:** when the new data dir holds no state and `<exe dir>/data` does, the
  state entries (identity, calibration, SQLite, profile registry cache, methods and the smaller
  instrument records) are copied once, each through a staging name; the old files are never
  deleted; the log names every entry copied or failed.
- **Installer:** neither script creates `{app}\data` any more (an existing one is left alone). F3:
  the eGrabber `[Run]` step has `Check: EGrabberNotInstalled`, which also looks for
  `{commonpf64}\Euresys\eGrabber\bin`; the task stays ticked by default.
- **F4:** the main window title is `MIB Studio Qt <version>` (the version Help ▸ About shows), not
  "MIB Studio Qt Scaffold". File ▸ Open Logs Folder opens `AppBackend::logDir()`, which was wrong
  for dev builds and Linux.
- `config.json` was already writable: `ConfigPathManager` keeps it in
  `%LOCALAPPDATA%\MIB_Studio_Qt\include` for a Program Files install. Not moved.
- **Tests:** `backend.data_directory` (resolver branches on injected inputs, the real write probe,
  migration round trip and permission faults, AppBackend loading the migrated identity),
  `installers.data_dir_and_egrabber`, and `frontend.mainwindow_shutdown` checks the title and log
  dir. Only the rig proves the Windows install itself (non-elevated launch, LOCALAPPDATA writes,
  the eGrabber check against the real registry).
