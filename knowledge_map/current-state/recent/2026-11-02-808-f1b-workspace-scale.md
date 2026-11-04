## 2026-11-02 — F-1b: analysis workspace path at full scale (#808)

F-1b of the FlowJo fold-in (goals F01, F04, F05, F08, F12, F16).

- **Columnar source handle.** Stored files resolve to F-source/2 (descriptor, no
  rows; bindings unchanged). `AnalysisHost` takes a `ColumnResolver`; Studio and
  Review lend `RecordingColumns` through `ReviewJobs::withRecordingColumns` after
  re-verifying the full-file SHA-256 (any path; non-open files via a short-lived
  reader). Saved strategies, reports, plots, scatter, gallery, overlays,
  comparisons and libraries run on full recordings through one core
  (`src/backend/analysis/SourceCore.cpp`); F-source/1 rows stay for inline
  live/run-archive sources (≤100k).
- **Paged browse metadata.** `ReviewSession` counts rows at open and pages them
  (`Hdf5Service::readMetadataRange`): open +7.9 MiB at 300k events (was +174 MiB).
- **Contract.** Review ABI stays 10 (additive: `columnar: true`; unmerged #818).

Proof: `review.workspace_scale` (255k + 260k events: strategy, report, plot,
gallery, retain, apply, comparison = oracle; rows/columnar parity; 15.9 MiB peak
growth, bound 64 MiB). Evidence class: real backend, synthetic files.
Remaining: worker jobs ≤100k rows, real-recording run. See
[[services/AnalysisWorker]], [[services/ReviewSession]].
