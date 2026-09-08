Title: Tauri platform services — paths, preferences, shell logging, opener, updater verification (BE-9, issue #279)

Context:
- Ports shell responsibilities that lived in Qt frontend utilities to the
  Rust/Tauri shell (epic #246) — deliberately, not by retaining Qt helper
  code. This is the Linux-verifiable subset; Windows packaging/update smoke,
  installer launch/rollback, Sentry glue, and QSettings migration remain on
  #279.

Implementation Notes:
- **`desktop/src-tauri/src/platform.rs`** — `app_paths` (stable app-data/
  config/log/cache/documents locations from the `bio.yofo.mib-studio`
  identifier), `get_preferences`/`set_preferences` (one JSON document in the
  app-config dir, atomic temp+rename writes, survives webview-storage
  clearing), `shell_log(level, message)` (webview/JS log lines appended to
  `<app_log>/desktop-shell.log` so C++/Rust/frontend context correlates from
  one directory; no tokens).
- **`desktop/src-tauri/src/updater.rs`** — update-manifest verification that
  **fails closed**: `parse_manifest` rejects malformed JSON, missing/empty
  version/url, and non-64-hex digests; `verify_artifact` rejects any SHA-256
  mismatch before an installer could launch. Mirrors the Python
  `verify-update-manifest` release contract on the Rust side; unit tests
  cover valid/tampered/missing/malformed paths.
- **Opener actions** — `tauri-plugin-opener`, capability-scoped in
  `capabilities/default.json`: only `https://**` URLs and
  reveal-item-in-dir; no arbitrary process launch.
- **Shell wiring** — File ▸ Open Data Folder (reveals the app-data dir),
  Help ▸ Documentation (opens the published manual site), sidebar collapse
  persisted to the preferences file (with localStorage as the fast cache,
  restored at boot), and every log-drawer line mirrored to the shell log.

Verification:
- Desktop `cargo test`: 9 tests (4 slice round-trips + 5 updater fail-closed
  tests).
- Xvfb smoke green with the opener plugin initialized.
- `npm run build`, `gen_bridge_contract.py --check`, backend CTest lane,
  `check_docs.py` green.

Follow-ups (tracked on #279):
- Windows installer launch/rollback + packaging/update smoke tests.
- Legacy QSettings migration; update channel/version catalog UI.
- Sentry/crash correlation and Report a Problem action.
