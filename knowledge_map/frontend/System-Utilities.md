# System & Utilities

> Non-tab helpers used across the frontend.

**Source:** `src/frontend/system/`, `src/frontend/utils/`,
`src/frontend/widgets/`, `src/frontend/models/`

## System (`src/frontend/system/`)

- **`QtLogBridge`** — `mib::frontend::installQtLogBridge()` installs a
  `qInstallMessageHandler` that routes Qt's process-wide log stream into spdlog
  (criticals/fatals also go to Sentry via
  `[[../services/CrashReporter]]::captureMessage`). Moved out of the backend
  CrashReporter so the backend links no Qt (epic #246); `main.cpp` installs it
  after `CrashReporter::init()`.
- **`LutHttpFetcher`** — `mib::frontend::makeQtLutHttpGet()` returns a
  `backend::HttpGetFn` (a blocking QtNetwork GET: single-shot event loop +
  transfer timeout). `main.cpp` injects it via `AppBackend::setLutHttpFetcher`
  so the Qt-free backend catalog ([[../architecture/AppBackend]] LUT
  management, ADR 0002) can fetch the E-modulus LUT without linking
  `Qt6::Network`. A Tauri/Rust shell supplies its own fetcher through the same
  seam.
- **`AppConfigWatcher`** — `QFileSystemWatcher` over an external JSON
  config. Reloads config and propagates to
  [[../services/ProcessingService]] / [[ConfigTabs]] on change. See
  `docs/howto/live-config-reload.md`.
  **Acknowledged apply path (issue #364):**
  `applyProcessingDraft(ApplyProcessingDraftRequest)` → `ConfigApplyResult`
  (slot `onApplyProcessingDraft` + signal `processingDraftApplied`). It
  validates the merged result (inverted ranges refused), reads the document,
  refuses with `conflict` when the request's `baselineFingerprint` or the
  watcher's own `documentFingerprint()` differs from what is on disk (an
  external edit not yet reloaded is retained, never overwritten), writes
  **only the patched keys** through `ConfigDocumentStore::writeText`
  (unknown keys, other sections and untouched high-precision values are
  preserved), then applies the same patch to `ProcessingService` and
  reports `persisted` / `applied` separately (`applied` = read-back
  matches). Nothing is written for an empty patch, a missing/directory
  path or malformed JSON. A successful write updates the document
  fingerprint and emits one queued `configFileChanged`; `onFileChanged`
  skips events whose on-disk fingerprint equals the loaded document (the
  self-write echo), so nothing is applied or broadcast twice.
  `writeBackCameraConfig` uses the same checked store. The old
  `writeBackProcessingConfig` (rewrite the whole section from runtime) is
  gone. Guard: `frontend.config_apply`.
  Config loads also read the optional top-level `experiment_buffer_max_mb`
  (issue #370) into `ProcessingService::setMaxBufferedBytes` (0 = count-only
  backlog bound; default 512 MiB when the key is absent).
  On startup, when the app-managed `config.json` already exists,
  `mergeNewDefaultsIntoConfig` deep-merges any keys added to the bundled
  `:/defaults/config.json` by a newer build into the existing file
  (`frontend::jsonutil::mergeMissingDefaults`, in `JsonConfigMerge`),
  **preserving all existing user values**. This stops an updated install from
  drifting away from a fresh install when new config keys are introduced. An
  external user-chosen config path is never rewritten.
  Config loads also parse `camera.frame_delivery_mode`
  (`"everyFrame"`/`"latestFrame"`; missing or unknown values deterministically
  map to `everyFrame`) and apply it through
  `CaptureService::setConfig` — the only frontend `setConfig` call path —
  then emit `deliveryModeLoaded(FrameDeliveryMode)`.
  `writeBackCameraConfig(mode)` persists just that key back to the active
  config file (preserving all unrelated keys) and is wired by [[MainWindow]]
  to the [[ConnectTab]] delivery-mode combo.
- **`AutoUpdater`** — update check + channel/version selection; see
  `docs/howto/auto-update-r2.md` and `docs/howto/release-workflow.md`.
  - Channel persisted in `QSettings` (`Update/Channel`, `stable`|`beta`); the
    default is the **build's own channel**, derived from its full version via
    `UpdateCatalog::channelForVersion` (a `-beta.` suffix → beta), so a beta
    build opens on the beta channel and marks its own release "current". An
    explicit user choice still wins; `MIB_STUDIO_UPDATE_MANIFEST_URL` env
    override still wins over everything. `defaultManifestUrl` is channel-aware
    (`{channel}/latest.json`).
  - **Build version identity:** `applicationVersion` is set from
    `MIB_STUDIO_QT_VERSION_FULL` (the git tag *with* its `-beta.N` suffix), not
    the stripped `MIB_STUDIO_QT_VERSION`. Without the suffix a stable and a beta
    build were byte-identical in version, so the app could not tell which channel
    it was on (both showed the same "current version"). `MIBVersion.cmake`
    exposes both: `PROJECT_VERSION` (numeric, for `project()`/installers) and
    `PROJECT_VERSION_FULL` (with suffix, for runtime identity + Sentry release).
  - `fetchVersionIndex()` GETs `{channel}/index.json` and emits
    `versionIndexReady`/`versionIndexFailed`, parsed by `UpdateCatalog`.
  - `installVersion(entry)` maps a catalog `VersionEntry` onto the existing
    download → SHA-256 verify → elevated-install path. The silent startup
    auto-check (latest.json) is unchanged.
  - Driven by **`SoftwareUpdatesDialog`** (`dialogs/`), opened from **Help ▸
    Software Updates…**: channel dropdown + version list with rollback (a
    downgrade is confirmed via `UpdateCatalog::isDowngrade`).
- **`ProfileManager`** — profile catalog/metadata helper for
  `ConfigTabs`. Scans local profiles, lazily generates `profile.meta.json`,
  parses public R2 catalogs, computes SHA-256, stages/installs updates, and
  produces field-level JSON diffs for manual update review. An optional
  `processing_contract_version` is round-tripped through catalog/local
  metadata and marks the profile incompatible when it differs from the active
  core; it never selects a core.
  `camera.frame_delivery_mode` is classified medium-risk in profile diffs
  (`isMediumRiskPath`), and `configSourceForPath` buckets `camera.*` paths as
  Config (not "Camera script", which only matches the `camera_script*` keys).
- **`DeviceInitManager`** — runs [[../services/CameraControlService]]
  `discoverCameras()` off the UI thread. Emits a signal when discovery
  completes (including "no cameras found").
- **`PlaybackPanel`** — the scrub+preview widget used by [[PreviewPage]]
  and [[MainWindow]]. Owns a `QImage` display, ROI overlay, scrub slider,
  display-FPS throttle, and overlay mode (Off/Mask/Contours/Both).
  - The Space-bar shortcut / key press does **not** start or stop capture
    itself: `onToggleCapture()` emits `captureToggleRequested()`, which
    [[MainWindow]] routes through its `CameraController` so the experiment
    guard and duplicate-command protection apply to every route (issue #360).
  - Presentation is a bounded pull: `onTick` reads the latest frame /
    snapshot at the configured `display_fps`, so no UI queue can grow behind
    a slow display. `displayFramesPresented()` / `displayFramesSkipped()`
    (issue #370) count rendered frames and frames the display skipped —
    presentation counters only, shown in the Diagnostics dialog and never
    mixed with acquisition, processing or persistence loss.
  - Overlay cell color (blue=target / green=valid / red=invalid) only uses
    the live [[../services/ProcessingService]] `getLatestSnapshot()` while
    *following live*. When stopped/scrubbing/replaying buffered frames the
    snapshot is stale and unrelated to the on-screen frame, so the color is
    re-derived per displayed frame via `computeProcessedFrame()` — otherwise
    the cell stays stuck on the last live frame's color (usually red).

## Models (`src/frontend/models/`)

- **`JsonTableModel`** — table view of flattened JSON (used by
  [[ConfigTabs]]).
- **`HdfMetricsModel`** — `QAbstractTableModel` reading metadata from
  [[../services/Hdf5Service]] (`readValidMetadata`, `readInvalidMetadata`).
- **`RunStatusModel` / `UiAlertModel`** (`RunStatusModel.h`, issue #363) —
  `RunStatusModel` is the run-lifecycle projection (`RunPhase` Idle /
  CameraRunning / Starting / Running / Stopping / Saving / Complete / Failed,
  each with a distinct `runPhaseLabel`/`runPhaseGlyph`): `beginOperation`
  returns an operation id, `setPhase(phase, id)` rejects stale ids,
  `latchFailure(id, reason)` turns a later `Complete` into `Failed` until a
  new operation begins, `setIdlePhase` never hides a latched failure.
  `UiAlertModel` holds actionable alerts keyed `source.code`: `raise`
  aggregates repeats (count, first/last time, message refreshed, re-surfaces
  an acknowledged alert), `acknowledge`/`acknowledgeAll` hide from the banner
  without resolving, `resolve` is the owner's job, `headline()` is the
  highest-severity unacknowledged alert, `kMaxRetained = 50` with an
  `overflowDropped()` counter that never drops an unresolved error/critical.
  Consumed by [[MainWindow]]. Guard: `frontend.run_status_model` (pure,
  Qt Core only).
- **`ProcessingConfigDraft`** (`ProcessingConfigDraft.h`, issue #364) —
  pure draft model for the Monitoring tune panel: `TuneField` enumerates
  the 20 exposed fields with full label, unit, display decimals, group
  (Acceptance / TargetGroup / MultiImage) and JSON path
  (`image_processing/...`); `readTuneField`/`writeTuneField`,
  `tuneValuesEqual` (equality at display precision), `ProcessingConfigPatch`
  + `applyTunePatch` / `applyTunePatchToJson` / `tunePatchSatisfiedBy`,
  `validateTuneConfig`. The draft holds baseline + draft + changed-field
  set: `setField` (no-op for a re-typed displayed value; restores the exact
  baseline), `patch()` of changed fields only, `noteExternalBaseline` →
  Refreshed / Unchanged / Conflict / Deferred, `revert`, and the apply
  lifecycle `beginApply` (request id; 0 when clean/invalid/conflict/busy) →
  `completeApply(ConfigApplyResult)` (stale ids ignored; clean only on
  persisted && applied; "saved, not applied" explicit) / `failApply`.
  `stateText()` is the footer text. Also defines the request/result
  contract used by `AppConfigWatcher`. Guard: `frontend.config_draft`.

## Utils (`src/frontend/utils/`)

`BackgroundPreviewWidget`, `ConfigPathManager`, `EgrabberConfigParser`,
`FileIOUtils`, `JsonFlatten`, `JsonConfigMerge` (pure deep-merge of bundled
defaults into an existing `config.json`; tested by
`tests/frontend/json_config_merge_test.cpp`), `UpdateCatalog` (pure parse/sort
of a channel `index.json` into a newest-first version list + downgrade check;
tested by `tests/frontend/update_catalog_test.cpp`), `OverlayRenderer`,
`RoiManager`, `SidebarWidget`, `SimpleImageCanvas`, `StatisticsPanel`,
`StatsDisplayManager`.

- **`SidebarWidget`** — content-only hardware panel (issue #359): background
  preview, `StatisticsPanel`, `NanopositionerTab`, `SyringePumpTab` in one
  scroll area (both scrollbars as needed). Shrinkable size policy, compact
  floor 160 px; no toggle button, no width/visibility persistence — the
  main window's splitter owns geometry (see [[MainWindow]]).
- **`ConfigDocumentStore`** — checked document write (issue #361, reused by
  #364): `writeText(path, text, expectedFingerprint, force)` verifies the
  on-disk fingerprint against the loaded baseline first (conflict result,
  nothing written), then writes through `QSaveFile` with a checked
  `commit()`, returning `ConfigWriteResult{ok, conflict, error, fingerprint,
  bytesWritten}`. Atomic replacement, not a cross-process compare-and-swap.
  Pure companion `ConfigDocumentState` (`frontend/models/`) holds
  path/fingerprints/dirty/conflict/last-save. Guard:
  `frontend.config_document_state`.
- **`ElidingLabel`** — `QLabel` whose painted text is elided (`ElideMiddle`
  by default) while `fullText()`/tooltip/accessible description keep the
  value and a context menu copies it; `minimumSizeHint` depends on a fixed
  number of characters, `sizeHint` is bounded (≈60 chars). Use it for paths,
  profile names and status strings (issue #358).
- **`WindowGeometryPolicy`** — pure, QApplication-free decisions in
  device-independent pixels: `chooseScreen`, `clampToAvailable`,
  `defaultWindowRect`, `resolveWindowGeometry` (version/validity/removed
  monitor), `fitSidebarWidth` (preferred vs compact vs does-not-fit with the
  640 px workspace floor), `sanitizeSidebarPreferredWidth`. Constants:
  minimum window 900x560, default 1280x800, sidebar 200–1000 (default 300,
  compact 160).
- **`ApplicationSettings`** — establishes the stable `MIB Studio` /
  `MIB Studio Qt` identity before any default `QSettings` access. Its one-time,
  versioned migration copies every missing user key from the former
  `Unknown Organization` namespace, never overwrites stable values or deletes
  the legacy store, and marks completion only after a successful sync. Desktop
  startup fails closed if this cannot be written.
- **`ProcessingCoreSettings`** — synchronizes the complete explicit native-core
  selection as one pre-activation commit. It snapshots and restores the prior
  logical values if `QSettings::sync()` fails; the backend does not swap the
  kernel unless this helper returns success.

## Widgets (`src/frontend/widgets/`)

- **`ZoomableChartView`** — subclass of `QChartView` with scroll/zoom.
  Used by [[ExperimentMonitoringTab]] and [[HdfReviewTab]].
- **`RunStatusWidget`** (issue #363) — glyph + `ElidingLabel` bound to a
  `RunStatusModel` (`bind`); text carries the state, color is only a
  secondary cue; accessible name "Run state: …"; bounded width (≤ 260 px).
- **`AlertBanner`** (issue #363) — `QFrame` bound to a `UiAlertModel`: shows
  the headline alert with severity word, ×count, remediation and "+N more",
  a checkable **Details** button revealing a bounded (120 px) read-only list
  of every unresolved alert, and **Acknowledge** (= `acknowledgeAll()`).
  Hidden when nothing is unacknowledged; both buttons are keyboard focusable.
  Placed by [[MainWindow]] above the main tab widget.

## Gotchas

- `AppConfigWatcher` is still the source of truth for live config reloads,
  but programmatic write-back now emits a direct change notification after
  persisting the file so sibling tabs do not depend on filesystem watcher
  timing.
- Profile catalog checks are manual only. `ConfigTabs` now owns the user
  interaction, while `ProfileManager` handles the file/network/hash work and
  keeps local metadata synchronized with updated local profiles.

## Cross-thread Qt signals

- **`BackgroundCaptureNotifier`** (`src/backend/BackgroundCaptureNotifier.cpp`)
  — tiny `QObject` that bridges non-Qt thread callbacks (e.g.
  background auto-capture from [[../services/ProcessingService]]) to Qt
  signals on the main thread. Owned by [[../architecture/AppBackend]].
