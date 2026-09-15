Title: Paged HDF5 review metadata/metrics/images + CSV export jobs (BE-6, issue #276)

Context:
- Exposes the HDF5 review surface through the bridge (epic #246). Bridge
  schema **v9** (additive). Runs on the BE-1 operation primitive for jobs.

Implementation Notes:
- **`backend::review::writeMetricsCsv`**
  (`src/backend/recording/ReviewExport.cpp`) — Qt-free metrics CSV with the
  exact column set/precision of the Qt HdfReviewTab export (scientifically
  equivalent outputs). Cancellable via a progress callback; partial files are
  removed on cancel/failure; inputs are read-only.
- **Facade** — `fetchReviewMetadata` (recording vs experiment mode,
  times/counts, ROI, core provenance, background presence, per-dataset
  capabilities via `getDatasetInfo`), `fetchReviewMetricsPage` (bounded pages
  served from a metadata-only cache — one metadata read per loaded file,
  invalidated on every RecordingLoad; never image payloads),
  `fetchReviewImage` (single image/mask via `readImageByIndex` hyperslab
  reads — bounded memory on multi-GB files; datasets contract-pinned:
  ValidImage/InvalidImage/RecordedImage/ValidMask/InvalidMask), and
  `ReviewCommand::ExportMetricsCsv` — a tracked Export operation running on
  its own thread with its **own read-only `Hdf5Service` reader** (never races
  interactive reads, never touches the source), progress events, and
  first-terminal-state semantics; facade shutdown joins job threads.
  `RecordingLoad` now replaces an already-open file (Qt parity) and is
  rejected during an active experiment.
- **Bridge/Tauri/TS** — `fetch_review_metadata`,
  `fetch_review_metrics_page(valid, offset, count)`,
  `fetch_review_image(dataset, index)` (+ dedicated binary byte channel),
  `review_export_csv(path)`. Shell Review tab: metadata summary line,
  enabled Valid/Invalid Frames tabs with per-dataset image scrubbers, a real
  paged metrics table (50/page pager), and a working Export Metrics to CSV…
  job with completion/failure surfaced from OperationStatus events.

Verification:
- `mib-bridge cargo test review_metadata_pages_images_and_export_job`:
  metadata for recording files, bounded pages + out-of-range safety, image
  pull dims + invalid dataset/index safety, export job completes with the
  Qt-parity header and row count, failed job (unwritable path) reports
  Failed, leaves no partial file, and the source stays readable.
- Full backend CTest lane, desktop cargo tests, `npm run build`,
  `gen_bridge_contract.py --check`, `check_docs.py` green.

Follow-ups (tracked on #276):
- Batch metrics / mask regeneration / reanalysis jobs and image/chart export
  variants (same operation pattern; need the batch tools port).
- Reader cache/prefetch tuning + a multi-gigabyte soak on real data.
- Contour/overlay binary pulls.
