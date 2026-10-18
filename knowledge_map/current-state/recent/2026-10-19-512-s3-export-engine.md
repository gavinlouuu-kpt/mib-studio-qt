## 2026-10-19 — Export engine in the review core (#512 S3)

Track B (#717), plan [`2026-10-18-one-review-implementation.md`](../../../docs/exec-plans/active/2026-10-18-one-review-implementation.md)
slice S3. The facade's `review_export_json` worker (request parsing, one
[[HdfExportService]] run per source, the batch rule "continue on failure,
stop on cancel", the status JSON) moved verbatim into `mib_review_core` as
`backend/review/ExportEngine.h` (`parseExportRequest`, `runExportBatch`,
`runExportFile`, `export*StatusJson`). `BackendFacade` keeps the policy (live
factor and chart range as defaults, the loaded file as the default source,
single flight, the `Export` operation, the snapshot), and ReviewJobs' Export
Metrics / Export All / Batch export run through the same engine with their
own requests (recorded factor per source, shell chart snapshots,
`<base>_metrics_N.csv`). The two hosts' cancel models are both kept
(`ExportCancelPolling::Direct` for the facade, `AtProgress` for ReviewJobs).
The unused second CSV writer `review::writeMetricsCsv` (`ReviewExport.{h,cpp}`)
is deleted. New test `review.export_engine`; `backend.export_bridge_facade`
and `review.jobs` pass unchanged; a before/after characterization of every
status snapshot, event and output tree was identical. No contract change.
See [[ReviewSession]].
