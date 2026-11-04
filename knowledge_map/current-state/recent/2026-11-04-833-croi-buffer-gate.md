## 2026-11-04 — Camera window in the profile, buffer sized from it, Start blocked outside the supported range (#833)

[[architecture/ExperimentCoordinator]], [[services/ProcessingService]], [[camera/EGrabberCamera]],
[[services/CameraControlService]], [[frontend/OverviewTab]], [[frontend/ConfigTabs]], [[frontend/MainWindow]].

**Why.** The MIB rig crashed saving at 5000 fps (#828). Its camera window (cROI) was 1216x208,
larger than the default 512x96, and the experiment buffer was a fixed 512 MiB: at 505,856 bytes
per saved frame it held 1061 frames, 0.21 s. Nothing compared the rate with the camera's maximum
for the window or with the destination's write rate.

**What changed.**
- **The camera window is in the profile.** config.json `camera.window {width, height, offset_x,
  offset_y, camera_vendor, camera_model}` (`app/CameraWindowProfile.h`). The Experiment script is
  followed by the profile's window through the same eGrabber handle, so the profile wins; the
  script's window is read first and a disagreement is reported (`camera.window` Warn). A window
  made for another camera model is refused (Fail), never clipped. Save Profile records the window
  and the connected camera and writes the same values into the saved script; Overview edits update
  an existing section. `camera.window.*`, `roi.*` and `experiment_buffer_max_mb` are High risk in
  the profile diff. `target_fps` now has a reader.
- **The buffer is sized from the window and the rate** (`processing/ExperimentBufferSizing.h`, one
  pure function, constants in one struct): 2 s at the requested rate, at least 2 x the flush
  interval, the save path (buffer + 4 write-queue batches) at most 75% of the available RAM; the
  frame cap and the byte cap agree. Computed in every readiness evaluation, applied at Start.
  `experiment_buffer_max_mb` is an explicit override (absent/0 = automatic). The backlog warning
  names the cap that bound.
- **Start is blocked outside the supported range** (`app/RecordingCapacity.h`): `camera.rate`
  fails above the camera's `AcquisitionFrameRateMax` for the window (read by `EGrabberCamera` at
  start, with `CycleMinimumPeriod` as the requested rate); `storage.buffer` fails when the minimum
  hold does not fit the RAM ceiling; `storage.throughput` fails when a measured destination write
  rate (a short fsynced write of frame-size chunks, ≤ 32 MiB / 0.75 s, cached per directory and
  frame size) is below the floor stream (invalid sampling with no cells). Unknown values warn.
- **Qt:** a Recording capacity panel on the Overview tab and a summary beside Start, refreshed once
  per second from `ExperimentCoordinator::recordingCapacity()` (no disk write, no generation change).

**Tests.** `processing.experiment_buffer_sizing`, `backend.recording_capacity_gate`,
`backend.camera_window_profile`, `backend.recording_capacity_readiness`,
`frontend.profile_camera_window`. Three tests that used a 1,000,000-frame `buffer_threshold` to
suppress automatic flushes now use 20,000 (a million frames needs ~100 GiB for the minimum hold);
`integration.fatal_save_dialog_e2e` sets the rig's 512 MiB through the override. Task note:
`knowledge_map/task/2026-11-04-833-croi-buffer-gate.md`.
