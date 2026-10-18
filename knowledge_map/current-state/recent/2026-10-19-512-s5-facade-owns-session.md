## 2026-10-19 — The facade owns the ReviewSession (#512 S5)

Track B (#717), plan [`2026-10-18-one-review-implementation.md`](../../../docs/exec-plans/active/2026-10-18-one-review-implementation.md)
slice S5. `BackendFacade` holds one `std::shared_ptr<review::ReviewSession>`:
`load_recording`, `close_review`, `fetch_review_metadata`,
`fetch_review_metrics_page`, `fetch_review_image`,
`fetch_run_accounting("review")`, `fetch_review_charts_json` and the export's
default source read it, so a review file no longer sits in
`AppBackend::hdf5()` (the experiment writer's handle) and the facade keeps no
review state of its own (`loadedRecordingPath_`, `reviewMetricsLoaded_`,
`reviewValidMeta_`, `reviewInvalidMeta_`, `reviewMutex_` deleted). The guards
stay the facade's, inside the coordinator's idle transaction. `ReviewSession`
gained `framesPage` (metadata-only `ProcessedFrame`s for the facade's
`MonitoringObjectRow`) and `validMetadataReadable` (the charts' "Cannot read
valid-frame metrics"). Decision D1 (a): `AppBackend::setReviewSourceOpen`, set
by the facade, keeps the experiment readiness gate `storage.hdf5`, Start and
raw recording refused with byte-identical text while a review file is open
(HDF5 is not thread-safe, #722). No bridge or review contract change. New test
`backend.facade_review_session` (the pre-slice facade code as the oracle);
`review.session`, `backend.experiment_readiness` and `backend.facade_boundary`
extended. See [[ReviewSession]], [[ExperimentCoordinator]].
