# ConfigTabs

> Docked panel with multiple sub-tabs for tuning: processing config, JSON
> table viewer, camera JS script loader, ROI and monitoring settings. Also
> owns the profile selector/actions for local and R2-backed profile catalogs.

**Source:** `src/frontend/tabs/ConfigTabs.cpp`,
`include/frontend/tabs/ConfigTabs.h`
**Related:** [[../services/ProcessingService]]::`ProcessingConfig`,
[[../frontend/System-Utilities]] (`AppConfigWatcher`, `ProfileManager`),
[[Dialogs]] (`ProcessingSettingsDialog`, `MonitoringSettingsDialog`)

## Explicit editor state and bounded header (issue #361)

- **State is a value, not a label.** Each editable document (app
  `config.json`, camera script, MindVision JSON) has a
  `ConfigDocumentState` (`frontend/models/ConfigDocumentState.h`): active
  path, loaded/current SHA-256 fingerprints, `dirty` (content comparison —
  editing back to the baseline is clean), `conflict` (file changed elsewhere
  while dirty), last save outcome. `onExternalConfigFileChanged()` consults
  `jsonDoc_.markExternalChange()` — never a label's `isVisible()` — so a hidden
  tab, a compact inspector (#362) or a resize can never let an external
  reload overwrite local edits. `documentStateChanged()` is emitted on every
  transition; `appConfigDocument()` / `cameraScriptDocument()` /
  `mindVisionDocument()` expose the values read-only.
- **Checked saves.** `saveEditorToFile()` writes through
  [[System-Utilities]] `ConfigDocumentStore` (QSaveFile + verified commit).
  A stale on-disk baseline is detected before writing and asked about
  (refused in non-interactive/test mode); a failed or refused save keeps the
  document Dirty and shows `Last save failed: …`. Saved is shown only after
  verified persistence. Save (file) stays separate from *Apply to Camera*
  (JS/MindVision tabs); Verified is never claimed by this widget.
- **Bounded primary header** (`headerWidget_`, above the tab widget):
  `Profile:` combo (content-length policy, ≤ 420 px), compact state label
  (`ElidingLabel`: `Loaded / Edited (unsaved) / Saved / Conflict` +
  `incompatible` + profile tags), `Reset`, `Save`, and a native **More…**
  menu (`configMoreBtn`) holding Save as profile, Rename, Delete, Duplicate
  as local, Check for profile updates, Update selected, Show diff, Open
  another config.json, Use the default config.json, Show config as table.
  Second row: elided path (`appConfigPathLabel`); third: wrapping notices
  (`appConfigNotices`: dirty/conflict/last-save-failed/incompatible/update
  available). The 220 px status minimum is gone; long names/paths cannot
  widen the window (`frontend.config_tabs_state` asserts the minimum size
  hint is unchanged and ≤ the narrow inspector budget).
- **Passive vs intentional profile refresh.** `refreshProfilesList(bool
  loadSelection = false)` rebuilds the combo keeping the selection *by
  identity* and never reloads; only startup and post-mutation callers
  (delete/rename/update/duplicate) pass `true`. *Check for profile updates*
  is passive: it cannot reload over an edited document.
- **Reflow without data change.** `relayoutJsonSections(width)` (debounced
  100 ms from a viewport resize event filter) moves the existing section
  `QGroupBox`es into 1/2/3 columns (font-aware ≥ 260 px cards) without
  touching models, JSON, config or files; `refreshJsonTableModel()` uses the
  same `columnsForWidth()`. The JS and MindVision pages sit in scroll areas
  and the MindVision trigger/strobe form is a two-row grid, so their content
  never becomes window-width pressure.
- Task-oriented captions: *Processing & app config*, *Camera script
  (EGrabber)*, *Camera trigger & strobe (MindVision)* (file names in tab
  tooltips). Raw JSON/table editing is unchanged.

## Responsibility

- Let the user edit `ProcessingConfig` fields (thresholds, gates,
  multi-image, target-group, ring-ratio).
- Drive camera-side GenICam scripts via
  [[../services/CameraControlService]]`::applyScriptToDevice` (through
  `AppBackend::applyCameraScriptFromFile`).
- Parse EGrabber scripts using `utils/EgrabberConfigParser.cpp`.
- MindVision tab ("MindVision config (mindvisionConfig.json)"): JSON editor
  with Reset/Save/Browse/Clear (QSettings key
  `Config/ExternalMindVisionConfigPath`, default seeded from
  `:/defaults/mindvisionConfig.json`), **Apply to Camera** →
  `AppBackend::applyMindVisionConfigFromFile` (guarded on
  `isMindVisionCameraSelected()`; stops capture, rebuilds factory), **Soft
  Trigger** → `AppBackend::softTriggerCamera` (acquisition trigger — distinct
  from the sort-pulse buttons in [[ExperimentMonitoringTab]]), and a pulse
  generator group driving [[../services/PulseGeneratorService]]. The group
  separates **Port / bus settings / Slave address / Channel**: a
  `QSerialPortInfo`-populated port dropdown (system name + description +
  USB S/N + VID:PID) with an explicit Refresh, baud/data/parity/stop combos,
  Modbus address spin, a read-only **Scan** (addresses 1–16, worker thread,
  cancelable, classifies generators vs generic Modbus devices vs
  corrupt/collision responses; never writes), Connect (typed `LinkError`
  status on failure), channel, frequency 400–40000 Hz defaulting to 5000 Hz
  = the 5000 fps bench trigger rate, duty %, Set/Start/Stop. Settings persist
  in the QSettings group `PulseGenerator` (port name **plus USB
  serial/VID/PID** so a renamed `/dev/ttyUSB*` node re-resolves when the
  identity matches exactly one port; ambiguous matches force operator
  selection). `~ConfigTabs` cancels/joins any running scan thread.
- MindVision "Trigger & strobe parameters" form: combos/spinboxes for
  trigger mode, edge type, exposure (0.8–838860 µs = MV-XGC51 sensor range),
  trigger delay/jitter/count, strobe mode/delay/width/polarity. **Two-way
  synced with the JSON editor** (`syncMvFormFromJson` /
  `syncMvJsonFromForm`, 150 ms debounce on editor edits, `mvSyncGuard_`
  breaks recursion): widget edits rewrite only their keys into the current
  JSON (untouched keys like ROI/gain survive; QJson alphabetizes on
  rewrite); Save/Apply always read the editor text, so the form never
  bypasses the config file. Mid-edit invalid JSON leaves the form at its
  last good state.
- Render/edit JSON config using `models/JsonTableModel.cpp` and
  `utils/JsonFlatten.cpp`.
- Persist config via `utils/ConfigPathManager.cpp` and
  [[../frontend/System-Utilities]] `AppConfigWatcher`.
- Manage profiles, lazy `profile.meta.json` generation, manual catalog
  checks, checksum-verified updates, and field-level diffing through
  [[../frontend/System-Utilities]] `ProfileManager`.
- Profile catalogs for manual remote updates are published outside the app via
  `publish-profiles.py` to
  `https://updates.yofo.bio/profiles/<channel>/catalog.json`; see
  `docs/howto/auto-update-r2.md`.

## Gotchas

- Parameter-tuning panel must stay in sync bidirectionally with the config
  table (see recent fix in
  `git log`: "fix: sync param tuning panel with config table").
- External config changes while the JSON editor has unsaved edits now set a
  visible stale/conflict warning instead of silently leaving the editor and
  table behind the backend state.
- JSON table rebuild is shared with `frontend::jsonutil` so the round-trip
  semantics for nested objects, arrays of objects, and arrays of scalars
  stay testable in one place.
- Profile state is now schema-aware: local profiles get a generated
  `profile.meta.json`, remote-managed profiles show update/incompatibility
  state, and the manual update flow stages downloads before replacing local
  files.
- Some gates require their `enable_*` flag too (e.g.
  `enable_ring_ratio_check`). Editing thresholds alone won't change
  classification.
- See task `knowledge_map/task/2025-11-25-config-profiles.md` for the
  profile management history and current implementation notes.
- Bundled defaults now include top-level `config_schema_version`. Remote
  profile support should treat missing schema as legacy local config rather
  than rejecting existing user profiles at startup.
