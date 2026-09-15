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

## Illuminated Live View setup (#413)

MindVision exposure and Save remain visible; trigger/strobe/JSON and manual
generator controls are collapsed under Advanced — Hardware Setup.
Use XGC + R5D preset for Live View persists the selected serial identity/channel
and tested 5 kHz/100 µs exposure/100 µs strobe settings in the active camera
JSON. Save/reload stages the file through AppBackend without opening hardware.
Apply on a coordinated profile also stages only; capture owns SDK application.
Generator controls cannot change an active owned session. Settings edits require
capture stopped. See [workflow](../../docs/howto/illuminated-live-view.md).


### Everyday FPS adjustment

Requested FPS is visible beside Exposure for a saved illuminated rig. Stop capture,
change FPS, Save, and Play to apply it through the coordinated generator startup.
It edits `live_view.frequency_hz`, not the camera's free-running speed selector.
The generator supports 400–40000 Hz; this is not a camera throughput guarantee.
The bench-tested point is 5000 FPS at 512×96. Observe actual acquisition rate and
use Rigol for physical timing acceptance when commissioning another rate.

Changing FPS preserves the trigger's active duration by scaling saved duty with
frequency: the preset's 5000 Hz / 10% becomes 2500 Hz / 5%, retaining a requested
20 µs trigger pulse. Exposure and strobe width/delay are not silently changed.
Existing backend validation rejects exposure or strobe timing that exceeds the
new period, and invalid generator duty. Legacy/manual profiles leave FPS disabled.
The real-widget regression covers visibility, persistence, duty compensation,
unchanged exposure/strobe, and restoration after reopening.


### Separate setup from raw configuration

Normal MindVision use hides the config file path and raw editor. Opening Hardware
Setup shows the setting form without also showing JSON; an explicit “Edit raw
configuration (JSON)” toggle reveals the editor. Closing Hardware Setup closes
that editor too, without discarding edits. Saved illuminated rigs describe Save
as staging the next Play, not requiring a separate Apply to Camera operation.


### Automatic default rig setup (September 14 follow-up)

The bundled XGC/R5D profile now enables illuminated Live View with `port: "auto"`,
9600 8N1, address 1, channel 1, 1000 Hz / 2% (20 µs pulse), exposure 2 µs,
rising-edge external trigger and manual strobe 100 µs / zero delay with
polarity 0 (the setting that pulses OUT1 on this rig; see the September 15
measurements). The existing
single-camera discovery selects the camera; Start performs read-only discovery
of USB serial adapters at the configured address on the capture worker. Exactly
one generator-compatible response is required before normal gated startup.
No match or multiple matches produces a specific error; no output is enabled by
discovery. Channel/wiring cannot be discovered electronically: channel 1 is the
known rig preset, not an inferred connection. Custom address/serial/wiring uses
Hardware Setup as an exception. Auto mode re-discovers the adapter each start,
so port renumbering does not require manually saving a new path.

Fresh installs save the bundled profile automatically. Only a byte-structure-
equivalent historical bundled JSON profile at the default path is upgraded;
custom and external profiles are preserved. Explicit saved ports continue to
work unchanged. Discovery exceptions are recorded as camera startup failures
and pass through illumination cleanup. The earlier mandatory one-time manual
setup instructions apply only to custom or ambiguous rigs, not the default rig.
Hardware acceptance of this changed build remains outstanding.


### Save validation and migration rule (September 14, second pass)

`onSaveMv` runs `backend::camera::mindvision::parseConfig` on the editor text
before writing; a profile whose FPS cannot fit exposure/strobe, or whose
`live_view` link is malformed, is refused with the parser's message and the
file and staged profile stay unchanged. The default-profile upgrade is the
pure static `upgradedMindVisionDefault(current, bundled)`: only a JSON-equal
copy of the verbatim pre-#413 bundled profile is replaced by the bundled
preset. The Requested FPS spin box has keyboard tracking off and the
compensated duty is rounded to 0.01 %.

### Exposure default after rig sweep (September 15)

The bundled and rig-local exposure is 2 microseconds; the LED strobe remains 100 microseconds.
Two hardware runs at 2 microseconds sampled mean grey levels 163.6 and 167.3 out of 255
at approximately 1000 fps with zero reported transport loss. Longer exposures
reached saturation (10-100 microseconds). Exposure is directly editable in the visible
MindVision Exposure (µs) field: Stop, edit, Save, then Start Live View.
Custom saved profiles retain their values. Runtime sweep: `data/exposure-sweep/results.csv`.
