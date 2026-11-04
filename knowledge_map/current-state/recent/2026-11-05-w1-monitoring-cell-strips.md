## 2026-11-05 — Monitoring cell strips: valid and invalid thumbnails with rejection reasons (#866 MON-052 .. MON-055, bridge ABI 45)

The Tauri Monitoring sub-tab shows the Qt tab's two strips: the last 25 valid and the last 25 invalid
detections as 100 px thumbnails, five per row, the invalid ones with the short reasons under the image
and a tooltip with the measured value and range, and a "Show Overlay" checkbox per strip (mask tint and
outline: blue target, green valid, red invalid). See [[architecture/Desktop-Shell]] (frontend) and
[[services/MonitoringCellStore]] (backend), [[architecture/Rust-Bridge]] (ABI 45).

**Backend (ABI 44 -> 45, additive, no new command).** `MonitoringCellStore` keeps an invalid strip
beside the valid one (own 25-cell and 4 MiB caps, the strip's run and capture rules, cleared by
`monitoring_clear` and by a capture change) and the strip cap goes from 24 to 25 (25 worst-case crops
are 3.1 MiB, inside the byte cap). Each invalid cell carries `reasons: [{code, value, min, max}]` from
the shared `science::classifyInvalidReasons`, so the strip and the live invalid-reason histogram
cannot disagree. Where they are computed: the ring read (now one hold of the ring's lock for both
halves, `ProcessingService::visitMonitoringNewestPerRing`) copies six scalars per invalid row, and the
reasons are derived in `refreshStrip` / `pin`, outside every lock, with the config read once per
refresh; nothing is added to the acquisition/processing path. The config is the one in force when the
cell is first listed (within one poll of its detection), not at every redraw as Qt's tooltip reads
it; the area uses the row's own calibration factor. A frame with no measured centroid (0, 0) is
cropped at the image centre, not in a corner. Memory budget: `monitoring.cellStore` is now
25 + 25 + 16 cells, 3 x 4 MiB.

**Frontend.** `areas/monitoring/CellStrips.tsx`, `useMonitoringCells.ts` (the first consumer of
`bridge.fetchMonitoringCells` / `fetchMonitoringCellImage`), `cellStrips.ts` (labels, tooltips,
overlay and thumbnail pixels, the bounded cache). Visibility-gated polling, pulls only for cells the
cache lacks, four at a time, no per-frame work (the Qt bug #857 pattern is not ported).

**Tests.** C++ `backend.monitoring_cell_store` (invalid strip, caps, reasons per code, parity with the
classifier over 3 contracts x 64 configs x 32 rows), `e2e.monitoring_cells_facade`,
`processing.monitoring_cell_concurrency` (a config switcher fills the invalid half); Rust
`contract.rs` `monitoring_invalid_cells_end_to_end`, `monitoring_cells_are_described`;
`monitoringCells.test.ts`, `cellStrips.test.ts`, `CellStrips.dom.test.tsx`.
