## 2026-11-04 — The app exits after a Review session (#859)

After Review and close, Qt logged `AppBackend: shutdown complete` but the process kept one core at
100% and never exited. A live stack on win11-agent (the main thread in `~HdfReviewTab` →
`QTableView` destructor → `QAccessibleCache` → `~QPersistentModelIndex`) showed the cause: a UI
Automation client had made Qt create one accessible interface per metrics-table cell, and the dying
views released them at a cost that grows faster than the square of the cell count. The tab now
resets and detaches its metrics models at the start of its destructor ([[frontend/HdfReviewTab]]).
Bench A/B with identical steps: develop still running 60 s after close at 100% of a core; the fix
exits in 0.8 s. Guard: `frontend.hdf_review_a11y_teardown` (30 ms teardown; 42 s with the old
destructor). The HDF5 "2 open objects before close" warning is unrelated (two handles on one file).
