## 2026-11-04 — Tauri Review: Export FCS, Batch Export FCS and each file's px→µm factor (#867 PR 1)

The Qt tab's FCS 3.1 export (#576) reaches both React/Tauri Review surfaces, and MIB Studio's
Review stage stops exporting at the live px→µm factor (#854, matrix REVIEW-068).

- **Engine** (`backend/review/ExportEngine.h`): `review_export_json` accepts `format: "fcs"`
  (`fcs_frames`, default valid) and an `ExportSourcePolicy`: `conversion_factor_source:
  "recorded"` exports each file at its recorded factor (the request's, else the live one, only
  for a file that records none) and adds the factor, its source and the counts to each
  `results` entry; `refuse_recordings` skips raw recordings with the Qt wording; FCS always
  refuses them and, without an explicit destination, takes the first free
  `<root>/<base>[_N].fcs` pair (`HdfExportService::nextAvailableFcsPath`, the Qt rule). No
  bridge ABI change (additive within the command); old requests keep their status bytes.
- **YOFO Review** (review ABI 7): `review_export_fcs` / `review_batch_export_fcs`
  (`ReviewJobs::startExportFcs`, kind `ExportFcs` = 9; `BatchExportRequest::fcs`). Toolbar
  **Export FCS…** and **More… ▸ Batch Export FCS…** with the Qt dialog titles, the
  `<base>.fcs` default and the Qt success text. Kinds 7/8 (the facade's JSON jobs) are now
  listed in the contract so the kinds stay contiguous.
- **MIB Studio** (`exportControls.tsx`, `areas/review/ReviewTab.tsx`): **Export FCS…** and
  **Batch Export FCS…**; every export sends `conversion_factor_source: "recorded"` with the
  live factor as the fallback; batch metrics / FCS refuse recordings; the Qt dialog titles
  and default names (`<base>_metrics.csv`, `<base>.fcs`) in the remembered export folder
  (`localStorage` `mib.review.lastExportDir`, set on success); metrics / FCS / charts off for
  recording files; keep-partial is an option (default off, as Qt) instead of always on; the
  open dialog is the Qt "Open HDF File" (`.h5`/`.hdf5` + All Files).

Tests: `review.export_engine`, `review.jobs`, `backend.export_bridge_facade`,
`tests/review_bridge.rs`, `exportControls.test.tsx`, `exports.test.ts`, the `reviewIpc` golden
(two new flows, none changed). See [[services/ReviewSession]], [[services/HdfExportService]],
[[frontend/YofoReview]], [[architecture/Rust-Bridge]].
