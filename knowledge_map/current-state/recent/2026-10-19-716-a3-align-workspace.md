## 2026-10-19 — O2 Align in the opt-in workspace: image tools and quick controls (#716 A3)

Step A3 of track A (#716), work package P3 (exec plan), on the reviewed Penpot board O2.

- **Live image.** The workspace's Align stage shows the frames the classic preview already pulls
  (one sink after the classic draw; no second capture path) with fit, 1:1, zoom and pan, a pixel
  probe (coordinates and raw Mono8 value), a histogram and a row or column line profile through
  a pinned point, the ROI outline and "Freeze display". Inspection is bounded (at most 65,536
  pixels for the histogram, 1,024 profile points) and runs at most every 250 ms on the newest
  frame, off the delivery path (`FrameInspector`).
- **Quick controls, requested → readback**, through the classic tab's own commands: the camera ROI
  (`save_camera_roi`), the PZ7035 Run window (placed, applied by the switch to Run) or the
  processing ROI (`set_processing_roi`). Exposure and frame rate are read-only readbacks, the
  PZ7035 Run timing read-only from `fetch_run_fps`, gain unavailable (not bridged). Bounds and
  increments are checked before sending; pending until the write's own reply; a refusal shows
  its reason and restores the readback; no reply in 5 s times out and ignores the late reply.
  No new bridge command, no contract change.
- **Navigation still never acts.** The camera mode switch the classic Camera & Alignment tab
  makes on entry is a button in Align ("Show the full sensor" / "Switch to Align"), behind the
  same function (`applyCameraMode`). The navigation gate now allows exactly two added reads
  (Prepare's path-less readiness, Align's geometry read-back), lists ROI/config/script writes as
  forbidden, and proves the button and Write send exactly the classic commands.
- **Classic fixes on the way:** geometry reads that answer out of order are dropped, so a readback
  read after a save is never replaced by an older one; the classic Apply ROI and the quick
  control share `applyProcessingRoi`; `useCameraWindow.save` returns its outcome.

See [[architecture/Desktop-Shell]] ("Opt-in workspace", Align) and [[frontend/UI-Language-Kit]].
