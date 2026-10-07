## 2026-10-07 — Qt profile actions stay synchronized with processing (#583)

Watcher path changes notify Monitoring after loading; clearing/deleting a profile
repoints live processing to the default config. Rename updates metadata, and
catalog revision comparison prevents normalized installed JSON from leaving a
stale update banner. Offscreen regression coverage in `frontend.config_tabs_state`
uses local files for catalog installation. See [[frontend/ConfigTabs]],
[[frontend/System-Utilities]], and [[frontend/ExperimentMonitoringTab]].

The lifecycle regression clicks the actual confirmation buttons and uses unique
profile directory names so aborted runs cannot contaminate rename checks. With
these assertions unchanged, the test fails against the original product code
for Monitoring refresh, update status, rename metadata, and deletion/save.
