# Replay clip capture at experiment start (#463)

Status: active (2026-09-30) — plan only; no code has landed.

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
| Early stop | Experiment stops before the window closes → clip ends there, marked `truncated` |

The clip is contiguous by write index. A frame the reader could not copy
(`FrameReadOutcome::Overwritten` etc.) becomes an explicit gap record; the
clip is then marked `non_contiguous`, never silently shortened.

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
  `processingConfigSha256` and `backgroundSha256`; a mismatch marks the clip
  `config_unverified`.
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
  manifest.json        schema version, app/build/core/contract identity, run link
                       (output path, start generation, start wall clock), capture
                       rule + actual bounds hit, counts (copied / gaps / written /
                       failed), state (complete | truncated | non_contiguous |
                       config_unverified | failed), end reason
  frames/000000.png …  lossless Mono8, sorted names → usable as MIB_MOCK_CAMERA_DIR
  frames.jsonl         write index, device timestamp + descriptor, hostTimestampUs,
                       width/height/pitch/pixelFormat, gap records
  processing_config.json
  background.png
  live_results.jsonl
```

`manifest.json` is written at arm (state `capturing`) and atomically replaced
at the end; a clip left in `capturing` after a crash is reported as
`incomplete` by the reader tools.

## Replay

1. **In-app:** mock camera accepts a clip directory, replays frames with the
   recorded pacing and timestamps (today `MockCamera` re-stamps and paces at a
   fixed interval), and the clip's config and background are applied.
2. **Headless:** a script over the Python wheel reprocesses the clip with the
   recorded core contract and diffs against `live_results.jsonl`, reusing
   `scripts/run_processing_conformance.py` patterns.

## Acceptance criteria

- [ ] A mock-camera experiment produces a clip whose frames, metadata, config
      and background round-trip; config/background hashes match the run
      snapshot.
- [ ] Mock run → clip → headless rerun → per-frame results equal live results
      on processed frames (pipeline e2e).
- [ ] Clip ends at exactly 1000 frames at high fps and at 1 s at low fps;
      early experiment stop yields `truncated`.
- [ ] Forced ring overrun yields explicit gap records and `non_contiguous`;
      copied + gaps = frames in window.
- [ ] Disk full, unwritable directory, encoder failure, allocation failure and
      shutdown during write: experiment output and accounting unchanged, clip
      state truthful (fault injection).
- [ ] Start→Running latency and steady-state throughput with clips on vs off
      stay within an agreed ratio at the highest supported fps and largest
      frame size (latency budget).
- [ ] Rapid start/stop/restart and shutdown during capture pass the stress
      test and the TSan lane with watchdog-bounded joins.
- [ ] Clip buffer reported through `MemoryOwnerStats` with a declared bound.
- [ ] Service vault note, Threading-Model, user manual disclosure and
      Recent-Work updated; `check_docs.py` and `check_screenshots.py` pass.

## Decision log

- 2026-09-30: v1 is the start-of-run replay clip only; timeline and GUI
  screenshots move to a separate issue sequenced with #395.
- 2026-09-30: capture is automatic on every experiment and invisible to the
  operator.
- 2026-09-30: clip = first 1000 frames or 1 s, whichever comes first.
- 2026-09-30: backend-only, armed by `ExperimentCoordinator`; no shell work.

## Open decisions

- **Byte safety cap** — proposed 512 MB.
- **Retention** — always-on adds ~50–300 MB per run. Proposed: total cap with
  oldest-first deletion of clips only (never HDF5), configurable.
- **Disclosure** — proposed: no UI, a manual page stating clips are kept, and
  an admin setting to disable capture.
- **Mock-camera runs** — capture them too (needed for tests) and mark the
  source in the manifest; confirm this is wanted outside tests.

## Progress

- [ ] 1. Recorder: coordinator hook, reader/writer, manifest, budgets,
      free-space floor, live-result hook; round-trip, fault-injection,
      concurrency and latency tests.
- [ ] 2. Replay: mock camera loads a clip with recorded pacing; headless
      rerun script; e2e equality test.
- [ ] 3. Docs: service vault note, Threading-Model, manual disclosure,
      Recent-Work; move plan to `completed/`.

## Related

#463 (this issue), #393 / #395 (operator journal; timeline follows it),
#451 (HDF5 writer ownership), #367 (frame accounting), #370 (memory bounds),
#368 (timestamp validity), ADR 0001 (React/Tauri migration).
