# Dialogs

> Short-lived modal widgets. One note for all — each is small.

**Source:** `src/frontend/dialogs/`, `include/frontend/dialogs/`

| Dialog | Purpose | Surfaced by |
|---|---|---|
| `MockConfigDialog` | Pick mock camera folder, interval, loop | [[ConnectTab]] |
| `ProcessingSettingsDialog` | Edit `ProcessingConfig` (full form) | [[ConfigTabs]] / menu |
| `ProcessingCoreDialog` | Browse stable/beta processing-core history, prepare a verified native artifact, and activate it at a safe between-operation boundary. See [[ProcessingCoreDialog]]. | [[MainWindow]] Settings menu |
| `MonitoringSettingsDialog` | Scatter density (KDE) bandwidth factor, update interval and core contour percentage; reference contour actions (*Pin current* / *Clear*, immediate, not applied); fixed scatter/histogram axis ranges, histogram bin width. The KDE values persist via the tab (`QSettings` `Monitoring/Kde*`); the axis ranges do not. | [[ExperimentMonitoringTab]] |
| `BufferSaveDialog` | Save FrameStore frames to disk. Output Format group chooses single uncompressed AVI file (default) or TIFF folder (one `frame_NNNNNN.tiff` per frame). AVI mode swaps the browse button to a `getSaveFileName` flow with `.avi` filter and surfaces an FPS spinner (30 default, playback metadata only). On save, the output path auto-iterates with `_1`, `_2`, ... if the destination already exists (files or non-empty directories), so the dialog never overwrites. After a successful AVI save the confirmation dialog mentions that ImageJ/Fiji can open the file (no launcher — the user opens it themselves). Range selection (all/index/timestamp) and empty-frame filter apply to both formats. | [[PreviewPage]] |
| `ConversionFactorDialog` | Set pixel→μm conversion factor | [[PreviewPage]] |
| `FrameViewerDialog` | Popout frame inspector with overlay toggles | [[HdfReviewTab]], [[PreviewPage]] |
| `SyringePumpSettingsDialog` | Per-pump COM port, baud, Modbus address | [[SyringePumpTab]] |
| `BatchMaskDialog` | Re-generate masks from HDF5 range, whole HDF5 file, image folder, or AVI file via [[../services/ProcessingService]]'s `processBatch`. HDF5 input resolves `/recorded_frames/images` for recording files and can preserve source indices/timestamps normalised to the first regenerated frame. Two-panel layout: controls on left, preview canvas on right. Uses `RoiDrawCanvas` for drag-to-draw ROI selection; ROI pre-populated from HDF5 `experiment_info`. Frame nav buttons (←/→) lazy-load one frame at a time for background selection. AVI source uses a cached `cv::VideoCapture` for preview seeks. If no manual background frame is selected, the dialog can build a synthetic background by averaging the lowest-change frames within each image tile. Overrides live pipeline ROI/background with dialog-selected values. | [[HdfReviewTab]] |
| `RoiDrawCanvas` (util widget) | Displays a `QImage` scaled to fit and lets the user drag a rectangle to define an ROI in image coordinates. Emits `roiChanged(QRect)` on release. Owned by `BatchMaskDialog`. Source: `src/frontend/utils/RoiDrawCanvas.cpp` | `BatchMaskDialog` |

## Conventions

- Each dialog takes a non-owning reference to the relevant service or
  config struct and emits/returns an updated value on `accept()`.
- Dialogs do **not** spawn threads or open serial ports themselves — they
  only mutate config; opening happens in the owning tab. The processing-core
  dialog is the exception for bounded asynchronous HTTPS downloads; it does
  not start processing workers and delegates cache/load/activation to the
  backend boundary.
