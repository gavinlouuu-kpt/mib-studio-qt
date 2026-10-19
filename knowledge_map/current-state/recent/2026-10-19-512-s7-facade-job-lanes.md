## 2026-10-19 — The facade's review jobs run on ReviewJobs lanes (#512 S7, #742)

Track B (#717), plan [`2026-10-18-one-review-implementation.md`](../../../docs/exec-plans/active/2026-10-18-one-review-implementation.md)
slice S7, decision D7 as amended 2026-10-19. `ReviewJobs` gained two lanes
(export; processing = regenerate / density / core / reanalysis), single flight
per lane, and a per-host policy, `ReviewJobsOptions`, with one preset per host:
`forReviewBridge()` (YOFO Review, and the default) keeps one job of any kind at
a time until HDF5 is thread-safe (#722); `forFacade(host)` (MIB Studio) runs
one export and one reanalysis at once, as before, as the facade's own
operations (`beginOperation` is the host hook: operation ids, cancel flags,
the `Export` / `Reanalysis` kinds, `cancel_operation`, shutdown
cancellation). The facade's `review_export_json` / `review_reanalysis_json`
workers moved into ReviewJobs as `startExportJson` / `startReanalysisJson`;
each lane holds its status snapshot. `exportThread_`, `reanalysisThread_`,
`exportActive_`, `reanalysisActive_` and their mutexes and snapshots are gone
from the facade; `reviewJobThreads_` (pump scans and stage waiters, never a
review job) is now `operationWorkers_`. Events, status strings, refusals and
cancel / shutdown behaviour are byte-identical for both hosts (the pre-S7
facade code is the oracle in `backend.facade_review_jobs`; a temporary
characterization of both hosts was identical). A worker never joins itself:
one whose own event starts its lane again or calls `shutdown()` is detached
and keeps the runner state it uses alive (shared with the workers) until it
ends, also if ReviewJobs is destroyed meanwhile (#742 review). New tests
`backend.facade_review_jobs`, `review.jobs_lanes` (they wait for the lanes'
own release, `ReviewJobsTestAccess::waitIdle`, never a sleep). Review round 2: callbacks may call `shutdown()` / `cancel()` but must not destroy the owner; a start overtaken by a `shutdown()` reports one Cancelled terminal without running, and `shutdown()` no longer waits for a start in progress (that wait could deadlock). No bridge or
review contract change. See [[ReviewSession]], [[Threading-Model]], [[Rust-Bridge]].
