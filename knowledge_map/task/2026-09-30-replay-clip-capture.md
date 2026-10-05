# Start-of-run replay clip (#463, slice 1)

Request: keep ~1000 images of every experiment so a run can be rerun at
small scale, invisibly to the operator. Issue #463 originally asked for a
study recorder (action timeline, sampled images, GUI screenshots); review
against `develop` rescoped v1 to this clip. Plan:
`docs/exec-plans/active/2026-09-30-replay-clip-capture.md`. Note:
[[../services/ReplayClipRecorder]].

## Decisions

- **Backend-only, armed by the coordinator.** Both shells start runs through
  [[../architecture/ExperimentCoordinator]]; the Qt shell calls services
  directly (~230 call sites) and Tauri has no operator workflow yet, so any
  shell instrumentation would be rebuilt or thrown away.
- **First 1000 frames or 1 s, whichever first** (user decision), plus a
  512 MB byte cap: camera rates in the docs range 200–5000 fps.
- **Direct call, not a status-callback subscription.** The coordinator's
  status callback is single-slot and already set by both `MainWindow` and
  `BackendFacade`.
- **Copy by write index, never `saveFramesToDisk`.** The latter holds the
  FrameStore's exclusive structural lock and would block `pushFrame`.
- **Hash-verify the reprocessing inputs.** The run snapshot stores only
  hashes; the clip saves content and records `config_verified`.
- **Skip, never wait.** A Start while the previous clip is still writing
  skips the new clip; Start must never block on a clip.
- **Retention (2026-10-05, developer-56 via merge coordination).** A clip
  belongs to its recording: `<stem>.replay-clip/` next to the run's HDF5,
  deleted only with the run, no expiry, never deleted to make room. Cap or
  reserve exceeded → the clip stops there, a warning is logged and the
  outcome goes into `/run_provenance @replay_clip_json`, written by the
  coordinator (the recorder never opens the experiment file).

## Verification

- `e2e.replay_clip` (mock camera over 64 ID-stamped frames) passed 5/5
  locally in Release and under a local TSan build (with
  `e2e.experiment_coordinator`); two deliberate mutations (drop every 10th
  frame; corrupt the handed-over config) each made it fail.
- Not yet: live per-frame results, on/off latency ratio, forced ring
  overrun, disk-full/encoder/allocation fault seams, manual disclosure.
