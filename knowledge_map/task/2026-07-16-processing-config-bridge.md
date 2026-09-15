Title: Processing config round-trip, ROI/background, core identity bridge (BE-3, issue #273)

Context:
- Exposes the processing configuration surface through the bridge (epic #246).
  Bridge schema **v8** (additive). Profiles and config.json file load/save/
  watch are the explicit remainder of #273 (see Follow-ups).

Implementation Notes:
- **`backend::processing::config_json`**
  (`src/backend/processing/ProcessingConfigJson.cpp`) — Qt-free nlohmann
  (de)serialization of `services::ProcessingConfig` using the exact
  `image_processing` schema of config.json (same keys the Qt ConfigTabs
  read/write). Merge semantics: only keys present in the JSON overwrite;
  type mismatches fail with a message instead of default-substituting.
- **Facade** — `fetchProcessingConfigJson` returns the full lossless
  document: `image_processing`, `realtime_processing` (enabled/mode/batch/
  drop), `flush_interval`, `pixel_to_micron`, `roi`, `background_set`, and
  the monotonic `config_version` (from `ProcessingService::getConfigVersion`)
  for external-change detection. `ProcessingSettingsCommand` gains
  `configJson` (merge-apply; malformed docs fail the whole command without
  touching state) and `flushInterval`. Background image get/set/clear with
  binary transfer (`fetchBackgroundImage` → Mono8 `BackendFrame`,
  `setBackgroundImage` validates len == w*h). `fetchProcessingCoreStatus`
  exposes the active `ProcessingCoreIdentity` + admin pin/`pinSatisfied`.
  `isRealtimeEnabled` getter added to `ProcessingService`.
- **Bridge/Tauri/TS** — `fetch/apply_processing_config_json`,
  `set_processing_roi`, `fetch_background_image`/`set_background_image`
  (binary)/`clear_background_image`, `fetch_processing_core_status`. The
  Tauri layer adds `set_background_from_current_frame` (operator "Set
  Background": pixels stay on the Rust side) and a separate background byte
  channel. The shell gains: ROI numeric editing (Overview toolbar + Clear
  ROI), Set/Clear Background in Preview, a live config-document editor with
  merge Apply + Reload, a core-identity/pin status line, and a live
  background indicator in the sidebar.

Verification:
- `mib-bridge cargo test processing_config_roundtrip_and_core_status`:
  lossless round-trip, merge semantics (absent keys keep values), malformed/
  type-mismatch documents fail without state changes, config_version
  advances, ROI round-trip, binary background set/get/clear with byte
  equality + bad-length rejection, core status observability.
- Full backend CTest lane, desktop cargo tests, `npm run build`,
  `gen_bridge_contract.py --check`, `check_docs.py` green.

Follow-ups (tracked on #273):
- Profile list/select/save/rename/delete/diff/duplicate port (Qt
  `ProfileManager` → Qt-free store; depends on BE-9 path services for the
  user-config directory).
- config.json file load/save/watch without Qt utilities (same dependency).
- Core activation rejection during incompatible operations lands with the
  #236 registry work.
