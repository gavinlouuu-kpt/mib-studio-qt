Title: Auto-fit processing ROI from background (issue #295)

Context:
- Including the microfluidic channel walls in the processing ROI injects noise
  (spurious wall-edge contours) and defeats the empty-frame fast path; cropping
  too tight clips real cells via the border check. The ROI is a *window* that
  must clear the walls without cutting the cell band.
- Measured on the real `gavinlouuu/512x96stream` mock stream (issue #295):
  full-frame ROI ~2.3x slower and ~10x more contours vs a wall-avoiding window;
  per-pixel adaptive suppression (temporal variance / occupancy) does NOT work
  because the wall noise is not pixel-locked. An ROI derived from the background
  structure does — so automate the ROI instead of asking the user to draw it.

Implementation Notes:
- New pure detector `backend::processing::detectChannelRoi`
  (`include/backend/processing/ChannelRoiDetect.h`,
  `src/backend/processing/ChannelRoiDetect.cpp`): OpenCV-only, Qt-free, part of
  the `mib_processing` core. Mean vertical-gradient (`cv::Sobel` + `cv::reduce`)
  row profile → rows above `wallGradientRatio` × central baseline are walls →
  largest contiguous non-wall band, trimmed by `marginRows`. Fails safe to the
  full frame on empty/flat/ambiguous input (incl. a peak-relative fallback when
  a perfectly flat channel makes the ratio test degenerate).
- `ProcessingConfig` gains `auto_roi_from_background` (default false),
  `auto_roi_wall_gradient_ratio` (2.5), `auto_roi_wall_margin` (1). Threaded
  through `AppConfigWatcher` read + write (`image_processing`).
- `ProcessingService::computeAutoRoiFromBackground` maps config → params and
  runs the detector. `setRealtimeBackgroundGray` — the single chokepoint every
  background capture (manual + auto-background, all loop variants) funnels
  through — applies the ROI via `setRealtimeRoi` (after releasing `rtMutex_`, so
  no re-entrant lock) and fires the new `SuggestedRoiCallback`.

Verification:
- `processing.channel_roi_detect` (`tests/processing/channel_roi_detect_test.cpp`):
  detector excludes wall bands / keeps centre, flat→full, empty→0x0, margin
  monotonicity, and the config-gated service path (off→full-frame sentinel,
  on→matches detector). Also validated standalone against the real HF
  background (band rows 20–65: walls excluded, cell band 34–57 retained).

Logging:
- Uses spdlog exclusively (`SPDLOG_INFO` on ROI apply).

Follow-ups:
- Surface `SuggestedRoiCallback` in the frontend (BackendFacade event +
  ProcessingSettingsDialog reflection) so the auto-chosen ROI shows in the UI.
  Backend applies the ROI directly today.
- Optional: extend detection to vertical walls (columns) if a channel is ever
  oriented that way; current detector is horizontal-channel only.
