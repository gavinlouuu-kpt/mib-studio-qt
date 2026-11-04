# 2026-11-04 — #833 camera window (cROI) in the profile, buffer sized from it, Start gated

Issue #833 (root cause of #828; #829 stopped the GUI crash). First target: the Qt app with
eGrabber. Other camera backends compile and behave as before except for the shared, trivial
parts (the `CameraWindow` / `CameraAcquisitionReport` types and a default `acquisitionReport()`
that reports nothing).

## Decisions (Gavin, not reopened)

1. The camera window belongs in the profile.
2. When the window changes, the experiment buffer is resized automatically for that window and rate.
3. Outside the supported range, Start is blocked (a Fail gate).
4. Qt + eGrabber first.

## Design as built

**Profile section** (`app/CameraWindowProfile.h`, parsed once by `parseAcquisitionProfile`, used
by the Qt `AppConfigWatcher` and the backend `ConfigDocumentApply`):

```json
"camera": { "window": { "width": 1216, "height": 208, "offset_x": 368, "offset_y": 616,
                        "camera_vendor": "SVS-Vistek GmbH", "camera_model": "MC2066" } }
```

- **Precedence with the camera script.** `AppBackend::applyCameraScriptFromFile(path, err,
  CameraScriptRole::Experiment)` runs the script, stops acquisition, reads the window the script
  left, then (`CameraControlService::applyScriptToDevice`) writes the profile window through the
  same short-lived handle: offsets to 0, size, offsets, read back. The profile wins; a differing
  script window is logged and reported by `camera.window` (Warn, both windows named). The Overview
  script (full sensor) uses `CameraScriptRole::Overview` and gets no window. `EGrabberCamera`
  never writes the window (it would crop the Overview).
- **Camera compatibility.** `cameraModelMismatch`: a window naming another model (or the same
  model from another vendor) is not applied; `camera.window` fails. A window naming no camera is
  applied and flagged (Warn).
- **Save Profile** (`ConfigTabs::onSaveProfile`): `ProfileManager::cameraWindowForSave` (the
  editor document's own window, else the applied profile window, else the script's) + the
  connected camera, written with `withCameraWindow`; the saved script's Width/Height/Offset lines
  are rewritten to match.
- **Overview edits** write the script as before and, through MainWindow,
  `AppConfigWatcher::writeBackCameraWindow` into an existing `camera.window`.
- **Risk:** `camera.window.*`, `roi`, `roi.*`, `experiment_buffer_max_mb` are High.

**Buffer sizing** (`processing/ExperimentBufferSizing.h`, pure `planExperimentBuffer`):

| Constant | Value | Why |
|---|---|---|
| `kTargetBacklogSeconds` | 2.0 s | hold 2 s at the requested rate if every frame were saved |
| `kWarnBacklogSeconds` | 1.0 s | `storage.buffer` warns below it |
| `kFlushFloorMultiple` | 2 | floor and minimum hold = 2 x flush interval (one batch filling, one handed over) |
| `kRamCeilingFraction` | 0.75 | of available RAM for buffer + write queue (BufferSaveDialog's 75% precedent, applied to the whole save path) |
| `kWriteQueueBatches` | 4 | `HdfWriteQueue` 3 slots + the batch being written, each one flush interval |
| unknown rate | 4 x interval, 1000..5000 | the pre-#833 frame cap |
| no window | 512 MiB | the pre-#833 byte cap |

Peak = bytes/frame x (N + 4 x min(F, N/2)). The RAM is read with the camera running (Start needs
it), so the FrameStore ring and the app are already outside "available"; 25% stays for the OS
write cache and other programs. bytes = N x bytes/frame (caps agree). The override replaces the
target and is cut to the ceiling too.

**Gates** (`app/RecordingCapacity.h`): `camera.window`, `camera.rate`, `storage.buffer`,
`storage.throughput` (rules in [[../architecture/ExperimentCoordinator]]). Invalidation key: the
camera report (window, requested and maximum fps), profile window, `target_fps`, window-apply
sequence, override, invalid sampling. Not the available RAM.

**Disk rate**: option 1 of the brief, a short sustained write (`app/DestinationWriteRate.h`):
frame-size records in ~4 MiB chunks, each fsynced (`_commit` on Windows), ≤ 32 MiB / 0.75 s, in
the destination's directory, removed afterwards; cached per (directory, frame size) for the
coordinator's lifetime; only measured once `storage.output` passed (so in Qt after the Save
dialog). Raw writes, not HDF5: the HDF5 writer adds a small per-frame cost, so the number is the
disk's (an upper bound for the writer). The previous run's own rate is not reused.

## Verified on hardware (read-only, win11-agent, Coaxlink Quad CXP-12 + EoSens 2.0MCX12)

`gentl genapi --ro --dump`: remote device `DeviceVendorName` = `SVS-Vistek GmbH`,
`DeviceModelName` = `MC2066`; `AcquisitionFrameRateMax` in AcquisitionControl; grabber device
module `CameraControlMethod` (RC) and `CycleMinimumPeriod` (float, µs). Nothing written. The
eGrabber code paths were syntax-checked with `-DMIB_HAS_EGRABBER=1` against
`/opt/euresys/egrabber-linux-x86_64-25.10.0.53/include`; they were not run on a camera.

## Tests

`processing.experiment_buffer_sizing`, `backend.recording_capacity_gate`,
`backend.camera_window_profile`, `backend.recording_capacity_readiness` (camera stub
`tests/support/acquisition_report_camera.h`, fake write-rate probe), `frontend.profile_camera_window`.
Adapted: `backend.experiment_readiness`, `frontend.experiment_fault_recovery` (flush interval
1,000,000 → 20,000), `backend.config_document_apply` and `integration.fatal_save_dialog_e2e`
(override instead of the service byte cap).

## Review (independent read, 2026-11-04)

Fixed: an edited `camera.window` in the editor was overwritten on Save Profile; the Overview
panel described the full-sensor overview session (now the planned rectangle, and Overview
sessions' rates are not used); failed write-rate measurements were repeated; a malformed window
dropped a valid `target_fps` in the Qt watcher; script integers are bounded; no window gate on
PL-science hosts; the rate source names the cycle trigger source. Kept: the device-module
identity fallback (the Coaxlink device module reports the camera's own vendor/model in the dump).

## Open

- MindVision and Aravis report no acquisition facts yet (their `camera.rate` warns). Aravis
  already publishes `frame_rate_max_hz`; wiring it is a small follow-up.
- `MainWindow`'s Overview `roiChanged` handler sets the processing ROI to the eGrabber camera
  window rectangle in sensor coordinates (pre-existing); not changed here.
- On the rig: apply the Experiment script once after loading a profile with `camera.window`, and
  check the camera.window / camera.rate / storage.throughput messages on D: and Z:.
- The write-rate probe runs on the thread that evaluates readiness (in Qt, the GUI thread at
  Start), bounded by 0.75 s plus one 4 MiB chunk; a hung network share blocks like the existing
  destination probes do.
- Pre-existing, seen while reading: `EGrabberCamera::start()`'s catch blocks call `stop()`, which
  takes the `stateMutex_` `start()` already holds (self-deadlock on a start exception).
