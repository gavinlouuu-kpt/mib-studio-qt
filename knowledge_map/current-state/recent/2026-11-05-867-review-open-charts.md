## 2026-11-05 — Tauri Review: open failure, status line, recording mode, lock and the file's factor (#867 PR 3)

Closes matrix rows REVIEW-003, -004, -006, -007, -049 and -068 and bugs #853 and #854 (charts half) for
both React/Tauri Review surfaces. See [[frontend/YofoReview]], [[services/ReviewSession]],
[[architecture/Desktop-Shell]], [[architecture/Rust-Bridge]].

- **#853.** The Regenerate masks panel sits inside a `ViewerGate` (a `display: contents` fieldset), so
  `.subtab-body > .config-group { flex: 0 0 auto }` no longer matched it and it covered the Valid /
  Invalid / Charts views. The rule now also matches `.subtab-body > fieldset.viewer-gate > .config-group`;
  `styles/subtabBodyGroup.test.tsx` takes the rule from `widgets.css` and fails if the gated group is
  not matched.
- **REVIEW-068 / #854.** MIB Studio's `fetch_review_charts_json` charted with the live factor. It now uses
  the factor the file recorded (`ReviewSession::ValidSet` carries `pixelToMicron` /
  `pixelToMicronFromFile`, read in the rows' snapshot), the live one only for a file that records none, and
  says which (`pixel_to_micron_source` "recorded" | "live"; the chart text no longer claims "current
  backend setting, same as Qt"). `fetch_review_metadata` gains `pixel_to_micron`,
  `pixel_to_micron_from_file` and `filtered_frames` (additive; bridge ABI stays 44), and the metrics table
  gets Qt's "Area (µm²)" column (2 dp) at that factor.
- **REVIEW-003 / -004.** A file that cannot be opened shows Qt's critical box "Cannot Open HDF5 File"
  ("Failed to open:\n<path>\n\n<detail>": "File not found." or the interrupted-write text with the
  `h5recover` hint) on both surfaces, and the status label reads "Ready", "Loading...", "Error loading
  file", "Valid: N, Invalid: M" or "Recording: N frames, M empty skipped". `ReviewSession::open` now says
  "File not found: <path>" or "Cannot read the HDF5 file (corrupt or not an HDF5 review file): <path>"
  (Qt's `QFile::exists`), and the facade's load result says "Recording load failed: file not found" for a
  missing file (the corrupt case keeps the pinned "Recording load failed"). YOFO Review also drops the
  previous file's info after a failed open: the backend had already closed it.
- **REVIEW-006 (MIB Studio).** A recording file shows one "Frames" tab (no Valid / Invalid tabs), Charts,
  Export Metrics and Export Charts disabled, an Index / Timestamp table and the status "Recording: N
  frames, M empty skipped".
- **REVIEW-007.** While an experiment is active (or raw frames are recorded) the Review stage is a disabled
  fieldset: Select HDF File and Close File say why, with Qt's "Review tab is disabled while an experiment is
  active. Please stop the experiment first." The stage-tab button belongs to the shell area and is not locked.
- **REVIEW-049.** YOFO Batch Export All wrote no chart images per file. `startBatchExport` now sets
  `generateReviewCharts` and each file's recorded ring-ratio range, so every experiment file's folder holds
  the backend-rendered `scatter_plot.tiff` and `histogram.tiff` (decision D5; Qt names the second
  `ring_width_histogram.tiff`). Recordings still get images only.
- A failed `fetch_run_accounting("review")` is now logged instead of silently hiding the outcome notice.

Tests: `review.charts_json`, `review.session` (validSet factor, open messages), `review.jobs` (batch charts,
mutation-checked), `backend.facade_review_session` (charts and metadata at a recorded factor, the load
message), `ReviewTab.test.tsx`, `useReview.test.tsx`, `reviewOpen.test.tsx`, `reviewOpenPanel.test.tsx`,
`ReviewCharts.test.tsx`. `tools/review_parity/compare.py` still expects the old D3 outcome (see YofoReview).
