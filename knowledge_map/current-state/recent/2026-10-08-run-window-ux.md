## 2026-10-08 — The Run window and a refused camera mode are shown to the operator (PZ7035)

Three gaps in the Camera & Alignment → Experiment hand-over, found when answering whether the
window propagates: (1) with no placed window, opening Experiment only wrote a log line; the tab now
shows "Place the 512×96 run window in Camera & Alignment" with a button that goes there, the mode
switch waits until a window is placed, and Start is disabled with the same reason; (2) a refused
Align/Run switch was log-only; the tab now shows the backend's reason with Retry, and Start is
disabled with it; (3) the window was lost on a page reload: `fetch_instrument_status.mode` now has
`run_set` (true once a Run switch succeeded in the backend process), and the UI restores its window
from `run_x`/`run_y` when it is set and nothing was placed yet. The backend still keeps the window
in memory only. Start Experiment on the PZ7035 no longer requires a running camera stream (Run stops
the producer); the backend `instrument.mode` gate is the check. The window state moved to
`cameraWindow.ts`, with tests for restore, placement and save-on-release for the non-PZ7035 ROI path
(none existed). Additive JSON field, bridge ABI stays 31. See [[architecture/Desktop-Shell]],
[[architecture/Rust-Bridge]].
