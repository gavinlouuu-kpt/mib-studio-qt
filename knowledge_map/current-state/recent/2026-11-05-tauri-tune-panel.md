## 2026-11-05 — Monitoring tune panel in the Tauri app, live tuning during a run (#866)

The React/Tauri Monitoring sub-tab has the Qt tune panel (`desktop/src/areas/monitoring/TunePanel.tsx`,
`tuneDraft.ts`, `useTune.ts`): the cell acceptance criteria, the target group gate with its Young's modulus
range and the multi-image setting, with Qt's ranges, steps and units, a `*` and tooltip on unapplied rows,
the Applied / N unapplied changes / Invalid / Conflict / Applying / Not applied / Saved, not applied footer,
Apply and Revert. It replaces the "Processing settings" stub (epic #866, rows MON-069..081). Apply goes to
the open checked configuration document when it agrees with the running configuration, else to the running
configuration only, and says so.

Backend: the cell acceptance and target group criteria now apply while an experiment is Active
(`ExperimentCoordinator::withTuneConfiguration`, `processingPatchIsTuneOnly`), through both
`apply_processing_config_json` and `apply_config_document`, as Qt does (MON-080). Every other key,
`multi_image` included, Starting, Stopping, Failed, a run being stopped and a PL instrument stay locked. No
bridge ABI change. Test `backend.experiment_tune_apply`. See [[ExperimentCoordinator]], [[architecture/Rust-Bridge]]
and [[architecture/Desktop-Shell]].
