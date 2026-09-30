# Replay clip capture at experiment start (#463)

Status: active (2026-09-30) — slice 1 recorder + mock-camera e2e landed;
live per-frame results, latency ratio test and paced replay next.

Rescopes issue #463. The issue asked for a study recorder (user-action
timeline, sampled camera images, GUI screenshots). Review against `develop`
(`e1c2bf3`) and the React/Tauri migration (ADR 0001) narrowed v1 to the part
that is most useful and survives the migration: a small, rerunnable clip of
raw frames taken automatically at the start of every experiment.

## Goal

Every experiment silently saves the first **1000 frames or 1 second of
frames, whichever comes first**, together with everything needed to reprocess
them: the processing configuration, background, processing core identity and
the live per-frame results. A researcher can rerun that clip in the app (mock
camera) or headless (Python wheel) and compare the rerun with what happened
live. The operator sees no difference: no control, no prompt, no added start
latency, and no change to acquisition, processing or HDF5 recording.

Non-goals (v1): user-action timeline, GUI screenshots, sampling beyond the
start clip, network upload, and the #393/#395 operator journal. The timeline
moves to its own issue, sequenced with #395.

## Why backend-only

- Both shells start experiments through `ExperimentCoordinator`
  (`MainWindow` and `BackendFacade` both call `backend_.experiment()`), so a
  coordinator-owned trigger covers Qt today and Tauri later with no shell work.
- The Qt shell calls services directly (~230 call sites) and Tauri has not
  started any operator workflow yet, so any per-shell instrumentation would be
  rebuilt or thrown away.
- All threading stays in `mib_backend`, which is what the TSan lane builds.

## Capture rule

| Parameter | Value |
|---|---|
| Trigger | Coordinator enters `Running` |
| First frame | First write index committed after the transition (recorded) |
| Stop at | 1000 frames, **or** 1 s of host time since the first frame (`hostTimestampUs`), **or** the byte safety cap, whichever comes first |
| Byte safety cap | Proposed 512 MB (open decision) |
| Early stop | Experiment stops before the window closes → clip ends there, state `incomplete`, `end_reason: run_ended` |

The clip is contiguous by write index. A frame the reader could not copy
(`FrameReadOutcome::Overwritten` etc.) becomes an explicit gap record; the
clip is then marked `contiguous: false`, never silently shortened.

Expected sizes (Mono8): 512×96 → 49 MB for 1000 frames; 1184×240 → 284 MB.
At 200 fps the 1 s bound gives 200 frames; at ≥1000 fps the 1000-frame bound
applies.

## Design

```
ExperimentCoordinator ──(Running, snapshot)──▶ ReplayClipRecorder::arm()   O(1), never fails Start
                                                  │
                    FrameStore ◀─ waitForFrame / readByWriteIndex ─ clip reader thread
                                                  │   (preallocated buffer, counted in MemoryOwnerStats)
                    ProcessingService ─ per-frame outcome for armed index range ─▶ live_results
                                                  │
                                         writer thread (low priority, rate-limited)
                                                  ▼
                                   data/replay-clips/<clip-id>/
```

- **Owner:** new `ReplayClipRecorder` owned by `AppBackend`, armed by
  `ExperimentCoordinator`; `AppBackend::shutdown()` stops it after the
  coordinator. Direct call rather than a status-callback subscription: the
  coordinator's status callback is single-slot and already contended by
  `MainWindow` and `BackendFacade`.
- **Reader:** copies by write index into a buffer reserved on the reader
  thread. Never uses `FrameStore::saveFramesToDisk` (it holds the exclusive
  structural lock and would block `pushFrame`). Does not use the capture
  inline frame callback.
- **Writer:** encodes lossless PNG and writes after (or behind) the reader on
  a low-priority, rate-limited thread so it does not compete with the HDF5
  writer's I/O in the first seconds of the run. Memory is released once
  written. Local disk only.
- **Config/background:** saved from the same state the coordinator froze in
  `RunConfigurationSnapshot`. The snapshot stores hashes, not content, so the
  recorder saves the content and verifies it against
  `processingConfigSha256`, `configJsonSha256` and `backgroundSha256`; a
  mismatch sets `config_verified: false`.
- **Live results:** per write index in the clip: processed or intentionally
  discarded (LatestFrame policy), valid/invalid object counts and object
  metrics. A rerun processes every frame, so comparison is restricted to
  frames the live pipeline processed.
- **Free-space floor:** before arming and while writing; below the floor the
  clip is skipped or abandoned and marked, so it can never starve HDF5.
- **HDF5:** untouched. The clip references the run (output path, start
  generation, start wall clock) instead of writing into the experiment file
  (#451 writer ownership).
- **Failure isolation:** any clip error (allocation, disk full, encoder,
  shutdown mid-write) is recorded in the manifest and logged; the experiment
  never fails, stalls or reports an error because of a clip.

## Clip layout

```
data/replay-clips/<utc-start>-g<startGeneration>/
  manifest.json          schema version, run link (output path, generations, wall
                         clock), core/contract identity, capture rule, counts
                         (window / copied / gaps / written / write failures / bytes),
                         state (complete | incomplete | failed), end_reason
                         (frame_limit | duration_limit | byte_limit | run_ended |
                         shutdown), contiguous, config_verified + hashes
  frames/000000.png …    lossless Mono8, named by offset → usable as MIB_MOCK_CAMERA_DIR
  frames.jsonl           per index: write index, device timestamp, hostTimestampUs,
                         width/height/pitch/pixelFormat, status (written | gap
                         reason | write failure | abandoned)
  run_snapshot.json      runSnapshotToJson(run)
  processing_config.txt  canonical ProcessingConfig (hashes to the snapshot)
  config.json            raw config.json as last applied
  background.png
  live_results.jsonl     (slice 1b, not yet)
```

`manifest.json` is written in state `writing` before any frame and
atomically replaced at the end; a clip left in `writing` means the process
died mid-clip. Skipped clips (disabled, busy, no space, no frames) write
nothing.

## Replay

1. **In-app:** mock camera accepts a clip directory, replays frames with the
   recorded pacing and timestamps (today `MockCamera` re-stamps and paces at a
   fixed interval), and the clip's config and background are applied.
2. **Headless:** a script over the Python wheel reprocesses the clip with the
   recorded core contract and diffs against `live_results.jsonl`, reusing
   `scripts/run_processing_conformance.py` patterns.

## Acceptance criteria

- [x] A mock-camera experiment produces a clip whose frames, metadata, config
      and background round-trip; config/background hashes match the run
      snapshot (`e2e.replay_clip`).
- [ ] Mock run → clip → headless rerun → per-frame results equal live results
      on processed frames (pipeline e2e).
- [x] Clip ends at the frame bound and at the duration bound (tested with
      scaled bounds: 200 frames, 150 ms); early experiment stop yields
      `incomplete` / `run_ended` with the copied frames kept.
- [ ] Forced ring overrun yields explicit gap records and `contiguous: false`;
      copied + gaps = frames in window (code path exists, no forced-overrun
      test yet).
- [ ] Fault injection: insufficient free space and shutdown during capture
      are covered; disk full mid-write, unwritable directory, encoder failure
      and allocation failure still need injection seams.
- [ ] Start→Running latency and steady-state throughput with clips on vs off
      stay within an agreed ratio at the highest supported fps and largest
      frame size (latency budget).
- [ ] Rapid start/stop/restart and shutdown during capture pass the stress
      test and the TSan lane with watchdog-bounded joins (stress scenario in
      `e2e.replay_clip`; clean under a local TSan build together with
      `e2e.experiment_coordinator`; CI sanitizer lane still to confirm).
- [x] Clip buffer reported through `MemoryOwnerStats` with a declared bound.
- [ ] Service vault note, Threading-Model and Recent-Work updated (done);
      user manual disclosure still to write; `check_docs.py` passes.

## Decision log

- 2026-09-30: v1 is the start-of-run replay clip only; timeline and GUI
  screenshots move to a separate issue sequenced with #395.
- 2026-09-30: capture is automatic on every experiment and invisible to the
  operator.
- 2026-09-30: clip = first 1000 frames or 1 s, whichever comes first.
- 2026-09-30: backend-only, armed by `ExperimentCoordinator`; no shell work.
- 2026-09-30: a Start while the previous clip is still writing skips the new
  clip (logged) rather than waiting; Start must never block on a clip.
- 2026-09-30: proposed defaults adopted in code pending confirmation: 512 MB
  byte cap, 512 MB free-space reserve, 64 MB/s write throttle, 5 s shutdown
  drain, `MIB_REPLAY_CLIP=0` opt-out.

## Open decisions

- **Byte safety cap** — proposed 512 MB.
- **Retention** — always-on adds ~50–300 MB per run. Proposed: total cap with
  oldest-first deletion of clips only (never HDF5), configurable.
- **Disclosure** — proposed: no UI, a manual page stating clips are kept, and
  an admin setting to disable capture.
- **Mock-camera runs** — capture them too (needed for tests) and mark the
  source in the manifest; confirm this is wanted outside tests.

## Progress

- [x] 1a. Recorder: coordinator hook, reader/writer, manifest, budgets,
      free-space floor, memory owner; `e2e.replay_clip` (mock camera:
      frame/duration limits, pixel-exact order, hashes, replay through the
      mock camera, early stop, no space, disabled, rapid start/stop, shutdown
      mid-clip).
- [ ] 1b. Live per-frame results for the clip range (`live_results.jsonl`);
      on/off latency-ratio test at the highest fps and largest frame size.
- [ ] 2. Replay: mock camera loads a clip with recorded pacing; headless
      rerun script; e2e equality test.
- [ ] 3. Docs: service vault note, Threading-Model, manual disclosure,
      Recent-Work; move plan to `completed/`.

## Related

#463 (this issue), #393 / #395 (operator journal; timeline follows it),
#451 (HDF5 writer ownership), #367 (frame accounting), #370 (memory bounds),
#368 (timestamp validity), ADR 0001 (React/Tauri migration).
