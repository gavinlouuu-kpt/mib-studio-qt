# Live config reload and propagation

This app reloads `config.json` at runtime whenever it is modified and saved, and propagates changes to all relevant services and UI:

- Processing parameters (`image_processing`) are applied to `ProcessingService` so realtime processing and classification reflect the new pipeline.
- Flush interval (`buffer_threshold`) updates the round‑robin flush cadence.
- Experiment buffer size override (`experiment_buffer_max_mb`, optional; absent or `0` = sized automatically from the camera window and rate at every Start, #833) updates `ProcessingService::setExperimentBufferOverrideBytes`.
- Camera window (`camera.window`) and `target_fps` update AppBackend's acquisition profile (#833): the window reaches the camera with the next Experiment script; the rate is the fallback when the camera does not report its driven rate.
- Display refresh (`display_fps`) updates the `PlaybackPanel` timer without restarting the app.
- Autofocus parameters are applied to `AutofocusService::Config`.

How it works
- A `frontend::AppConfigWatcher` uses `QFileSystemWatcher` to watch the active config path. The active path respects `QSettings` key `Config/ExternalAppConfigPath`; otherwise it uses the app include path `../include/config.json` beside the executable.
- `ConfigTabs` emits `appConfigPathChanged` when the external path is changed (Browse… or Clear), and `PreviewPage` forwards this to the watcher to retarget.
- On change, the watcher:
  - Parses JSON via Qt.
  - Builds `ProcessingService::ProcessingConfig` from `image_processing.*` (blur size, background threshold, morphology, area/empty thresholds, filter toggles).
  - Calls `processing.setProcessingConfig(...)`.
  - Calls `processing.setFlushInterval(buffer_threshold)`.
  - Calls `playback.setDisplayFps(display_fps)` to adjust the UI refresh timer.
  - Maps autofocus keys to `AutofocusService::Config` and applies them.

Pipeline parity
- `ProcessingService::realtimeLoop` now uses config‑driven parameters for Gaussian blur, threshold, and morphology (kernel size and iterations).
- `PlaybackPanel::computeProcessedOverlay` uses the same config values from `ProcessingService::getProcessingConfig()` so overlay masks match the backend pipeline.

Notes
- Logging uses spdlog; no `std::cout` in app code.
- If the default include `config.json` doesn’t exist yet, it is seeded from `:/defaults/config.json`.
- This aligns with the workspace rule to prefer ready‑made SDK patterns for metrics/capture (see Euresys samples); runtime reload is additive to that.


