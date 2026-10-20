## 2026-10-20 — Per-row cell images for live Monitoring (#716 A6a)

Bridge ABI 41, additive. The backend now owns bounded copies of detected cells for the live
Monitoring / Live views: [[services/MonitoringCellStore]] keeps a **strip** of the most recent
valid cells (24 cells / 4 MiB, oldest evicted first, counted) and the operator's **pins** (16 /
4 MiB, never evicted to make room), each an owned Mono8 crop around the object (≤ 256 px) plus the
same crop of the mask. A cell is named by `(start_generation, capture_epoch, frame_index,
object_id)`: the monitoring ring now tags every row, under the lock the append already holds, with
the run active when it was appended (0 = outside a run, set by
[[architecture/ExperimentCoordinator]]) and with the capture its frame came from, so a row of one
run or capture can never be mistaken for the same frame index of the next. Pins belong to the
capture that made them: the epoch moves on at the moment the capture changes (a realtime start, a
camera mode switch even with realtime stopped, the camera opening or closing a session, another
processing ROI, a buffer resize), the strip and the pins of the old capture are dropped right then
(the listing's `last_capture_change` says why), and every read path refuses an earlier capture's
identity first: a pin is `CaptureMismatch`, an image pull the `CAPTURE_MISMATCH` error with the
plain reason. Cell geometry and snapshot centroids are in ROI image coordinates in both
realtime modes (async-batch rows are translated by their ROI origin). A pin survives ring and
strip eviction (not a capture change); a pin naming any run but the latest one started (or 0) is refused
(`RunMismatch`); every pin and the strip are dropped when the next run reaches Active; a row that
already left the ring is pinned with its recorded measurements and a plain reason
(`PinnedWithoutImage`, from a bounded index of the rows the snapshot served), never a substitute
image. New commands (Tauri and the WebSocket server): `fetch_monitoring_cells`,
`monitoring_cell_pin` / `monitoring_cell_unpin` / `monitoring_cell_clear_pins` (control commands:
a viewer gets `VIEWER_ONLY`) and `fetch_monitoring_cell_packet` (binary frame packets, new pull
kinds 5 image / 6 mask; no pixels in JSON). Monitoring snapshot rows gain `start_generation`.
Nothing new runs on the acquisition path: the ring stores one more 24-byte tag per append; readers
hold the ring lock only for scalars and cv::Mat headers, crop outside every lock and drop the shares
under the ring lock again; image pulls copy under the store mutex, so every free follows its readers
in a lock order ThreadSanitizer can see (measured: ~1.4 µs hold per strip read, ~1.5 µs per release,
append p50 unchanged, realtime throughput ratio 0.983–1.011). The develop
UI does not use the commands yet (the workspace Live view, A6b, will); `desktop/src/monitoringCells.ts`
and the `bridge.ts` methods are the client. See [[architecture/Rust-Bridge]] (ABI 41),
[[services/ProcessingService]], [[architecture/Desktop-Shell]], [[diagnostics/MemoryBudget]]
(`monitoring.cellStore`). Guards: `backend.monitoring_cell_store`, `e2e.monitoring_cells_facade`,
`processing.monitoring_cell_pipeline` (crop pixels with an ROI origin in both modes; captures
across a FrameStore swap and a resize, old strip and pinned images refused at the restart), `processing.monitoring_cell_concurrency` (TSan lane),
`performance.monitoring_cell_hot_path`,
`contract.rs` `monitoring_cells_end_to_end`, `dispatch.rs` `monitoring_cells_through_dispatch`,
`ws.rs` `live_cells_over_the_websocket`, `monitoringCells.test.ts`.
