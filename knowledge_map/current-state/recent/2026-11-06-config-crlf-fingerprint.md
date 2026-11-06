## 2026-11-06 — Tune-panel Apply works on CRLF config files (#904), save dialog folder (#906), no profile rewrite on launch/close (#907)

The rig check of `v1.2.0-beta.d9d74fee` found every tune-panel **Apply changes** refused as a
configuration conflict on Windows, which blocked the Young's modulus gate (#831).

- **#904 root cause:** `AppConfigWatcher::applyFromPathWhenIdle` read the config with
  `QIODevice::Text`. Qt's Text read drops every `\r` (on all platforms), so the loaded document
  fingerprint was the SHA256 of LF bytes. The apply path and `ConfigDocumentStore::currentFingerprint`
  hash the raw CRLF bytes, so on a CRLF profile (all Windows profiles) both the baseline check and
  the watcher's own-write echo check always failed.
- **Fix:** `ConfigDocumentStore::readDocument` is the one binary read behind every config
  fingerprint (load, Apply baseline, on-disk check, echo check, camera write-backs). Writes stay
  binary UTF-8 with `\n`; `ConfigTabs::writeTextFile`/`readTextFile` now use the store too
  (atomic, no Windows-only CRLF translation). Existing CRLF profiles load and apply as they are and
  become LF at the app's next write. See [[frontend/System-Utilities]], [[frontend/ConfigTabs]].
- **#907:** the active profile's `config.json` was rewritten on every launch and close by
  `NanopositionerTab::saveConfig` (startup auto-connect, `aboutToQuit`) and
  `SyringePumpTab::saveConfig` (`aboutToQuit`): unconditional Text-mode rewrites. Both now read
  the raw bytes, skip the write when the JSON is unchanged, and write through the store. The
  `profile.meta.json` `camera_script_sha256` change is a separate, one-shot reconcile in
  `ProfileManager::scanLocalProfiles` (raw bytes on both sides); not changed here. See
  [[frontend/NanopositionerTab]], [[frontend/SyringePumpTab]].
- **#906:** the Start Experiment save dialog starts in the profile's `save_directory`, resolved
  under `<Documents>/MIB_Studio_Qt` when relative (it passed `""`, so Windows opened the install
  folder). See [[frontend/MainWindow]].
- **Tests:** `frontend.config_apply` section 5 (CRLF document: raw-bytes fingerprint, echo of a
  byte-identical rewrite, E-gate Apply twice, external CRLF edit conflict) and
  `frontend.nanopositioner_callback` (#907: unchanged CRLF profile keeps bytes and mtime) fail on
  the old code on Linux too; `frontend.config_document_state` pins `readDocument`;
  `frontend.experiment_save_location` covers #906. The rig re-check proves the Windows install.
