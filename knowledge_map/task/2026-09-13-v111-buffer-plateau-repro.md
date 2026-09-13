# v1.1.1 buffered-cell plateau reproduction

## Scope

2026-09-13 Discord report: valid and invalid buffered-cell counters stop changing
while cells continue flowing and the Qt application remains responsive. Saving
was suspected to have stopped. Reproduced a matching backend mechanism, not yet
confirmed against the user's active configuration or Windows UI.

Exact release: v1.1.1, ad82cf69d7b42eb79a72707cf6deb40a7805a03b.
Production code is unchanged. The existing experiment coordinator test has an
opt-in diagnostic mode, MIB_REPRO_BUFFER_CAP (bytes), using synthetic 512x96
ring frames at 1000 fps, inline realtime processing, lenient gates, no automatic
background, invalid sampling 1, and flush threshold 100 frames. The diagnostic
prints observations; it is not a pass/fail regression test for the suspected bug.

## Evidence

- 393216-byte cap: retained 4 valid / 0 invalid frames at every one-second
  sample for 5 seconds. Persistence admissions rose from 1002 to 5002 while
  committed remained zero and policy drops rose from 998 to 4998.
- Stop finalized with 5012 persistence admissions = 8 committed + 5004 policy
  drops, zero pending, reconciled. The extra admissions/flush during stopping
  explain why the final saved count exceeds the plateau count.
- 536870912-byte default-cap control: 4753 committed by the fifth sample,
  zero drops; final 5017 admissions = 5017 committed, zero pending.
- h5dump independently reopened both final files: images, masks and metadata
  each contain 8 rows for the small cap and 5017 rows for the control.
- Original e2e.experiment_coordinator CTest (without the env var) passes.

## Mechanism

ExperimentCoordinator::worker only periodically flushes when buffered frame
count reaches getFlushInterval(). ExperimentFrameBuffer separately refuses
admissions once its byte budget is exhausted. If that budget fits fewer frames
than the flush threshold, the count cannot reach the threshold. Capture and
processing continue, buffered counts plateau, writes wait for Stop, and new
admissions are dropped by policy. The production default byte budget is 512 MiB;
large images/multi-image series or a high flush threshold could hit the same
condition, but those settings have not been verified for the reported run.
A stable sampled buffer count alone is not proof: the healthy control also had
near-constant sampled counts, while its committed total advanced.

## Repeat

From the isolated worktree, after building mib_backend_tests:

```bash
mkdir -p data/buffer-repro
TMPDIR="$PWD/data/buffer-repro" MIB_REPRO_BUFFER_CAP=393216 QT_QPA_PLATFORM=offscreen timeout 40s build/repro/Release/mib_backend_tests experiment_coordinator_test
TMPDIR="$PWD/data/buffer-repro" MIB_REPRO_BUFFER_CAP=536870912 QT_QPA_PLATFORM=offscreen timeout 40s build/repro/Release/mib_backend_tests experiment_coordinator_test
```

Logs and HDF5 header snapshots: data/buffer-repro/{small-cap,default-cap}.log
and data/buffer-repro/{small-cap,default-cap}-hdf5-header.txt (local runtime artifacts; do not stage).
The final REPRO line identifies each preserved HDF5 path.

Next: capture the user's flush interval, ROI, multi-image count and backlog/drop
logs; implement a regression-first byte-pressure/partial-batch flush fix with
bounded-memory and accounting coverage. No production fix was made in this task.

## Expanded failure-pattern investigation (2026-09-13)

Production sources remain unchanged. Diagnostic knobs in the coordinator harness:
MIB_REPRO_FLUSH (default 100), MIB_REPRO_INTERVAL_MS (default 1),
MIB_REPRO_SERIES (default 1), MIB_REPRO_CHANGE_FLUSH (applied at third sample).
The harness now independently reloads persisted accounting and samples memory
300 ms after terminal status. Knobs apply only with MIB_REPRO_BUFFER_CAP set.

### Confirmed mock-pipeline patterns

1. **One payload exceeds the byte cap:** 49152-byte budget, 512x96 original +
   mask (98304 bytes). Both counts stay zero. 5001 persistence admissions are
   dropped; the final HDF5 opens but contains no image datasets.
2. **Polling-window loss even when threshold fits:** four-frame byte capacity,
   flush threshold 4, 1000 fps. 80 saved / 4923 dropped. At 10 fps, still 40 saved /
   10 dropped: the threshold is only checked on the coordinator's 250 ms tick,
   not on admission, so reaching the threshold does not immediately drain.
3. **Lowering threshold midrun is not recovery:** changing 100 to 4 at the third
   sample resumes writes, but final 37 saved / 4966 dropped. Earlier drops cannot
   be recovered, and polling-window losses persist.
4. **Oversized multi-image series:** ten-image series, four-frame-equivalent byte
   capacity. 500 complete series rejected, no image datasets. These persistence
   units are series/trigger records, not 500 raw camera frames. Also exposed the
   Stop race below.
5. **Benign partial batch:** 10 fps, threshold 100, ample 10 MiB budget, 5 seconds.
   Zero live commits, no drops, 50 buffered; normal Stop saves all 50. Delayed
   writes alone do not establish a failure.

All six scenario processes returned zero (diagnostic success, not assertions of
correct product behavior). h5dump reopened every output; nonempty image/mask/
metadata shapes matched committed counts. Machine-readable observations and
settings: data/buffer-repro/pattern-results.json. Execution script is local at
 data/buffer-repro/run_patterns.py.

### Confirmed multi-image Stop race

Use 10 fps, series length 100, 10 MiB budget and flush threshold 100. After five
seconds there are 50 source frames in an unfinished series. Stop reports
completion=complete with persisted=0/0 and closes the file. On the next camera
frame (~84 ms later in the first isolated run), ProcessingService's ROI-path
experiment-ended branch appends that incomplete 50-image series. At terminal
+300 ms: buffer=1, live persistence admissions=1, live pending=1; independently
reloaded disk accounting says admissions=0, pending=0. The file has no saved
series. This is distinct from byte-cap starvation and occurs with sufficient
memory and without a slow disk.

Cause: finalization flushes, calls endExperiment(), flushes any current
remainder, then closes. The realtime thread's pending multi-image series is
not necessarily handed off before that remainder check. The next processing
iteration can append after terminal status. A late unsaved buffer may then be
cleared on the next startExperiment(). The latter is source-based inference,
not a restart experiment performed here.

Reproduced twice with the same terminal/live/disk mismatch. h5dump verified
zero image datasets in both files. Logs: data/buffer-repro/series-stop-race.log
and series-stop-race-repeat.log; corresponding -hdf5-header.txt snapshots preserved.

### Queue-level fault probes (not hardware-disk reproduction)

Existing backend.hdf_write_queue, processing.fault_injection and
 e2e.experiment_coordinator tests pass. New standalone diagnostic source:
 tests/tools/recording_queue_failure_probe.cpp, compiled using:

```bash
c++ -std=c++17 -pthread -Iinclude -Itests tests/tools/recording_queue_failure_probe.cpp -o build/repro/recording_queue_failure_probe
build/repro/recording_queue_failure_probe
```

- Blocked writer + three queued batches + overflow: four accepted, one written,
  three accepted batches unwritten; one fatal callback, stop returns false.
  This is explicit fatal behavior, not the silent buffer-starvation path.
- Stop waits for the in-flight writer to return. The probe releases it after
  100 ms; source has an unbounded join, so a truly stuck disk write can keep
  finalization waiting. No actual indefinitely hung disk was induced.
- After a clean flushAndStop(), submit() incorrectly returns true; a second
  flushAndStop() also returns true although the new batch was never written.
  Queue API contract defect confirmed in isolation. Normal ProcessingService
  moves/destroys the queue on finishFlush(), so app-level reachability is NOT
  established.

### Priority for follow-up fixes

1. Synchronize multi-image stop handoff before final drain and accounting seal.
2. Make flushing byte-pressure/latency aware, with enough headroom for arrivals
   during scheduling/write delays; merely lowering a count threshold is insufficient.
3. Reject impossible payload configurations explicitly and surface runtime drops.
4. Guard post-stop queue submissions; separately investigate writer-error
   accounting lifetime (queue error currently lives in a queue moved out by
   finishFlush; no end-to-end failure-accounting repro yet).

These are targeted mock/queue results, not confirmation of the user's exact
Windows run. User ROI, series count, flush interval and runtime logs remain needed.

## Pre-start detection tests (2026-09-13)

Opt-in MIB_REPRO_PREFLIGHT_ONLY=1 evaluates current readiness without starting
an experiment. Three previously demonstrated unsafe configurations ALL report
ready=true on v1.1.1: 393216-byte buffer vs threshold 100; 49152-byte buffer vs
98304-byte single payload; 393216-byte buffer with ten-image series. Normal
512 MiB control also reports ready=true. Thus these are missing checks, not
failures detected by current readiness.

Two invalid destinations (existing directory as output; an existing TIFF used
as a parent directory) correctly report ready=false, storage.output=fail in
all four runs. Existing storage probe creates an empty file and checks 64 MiB
minimum free; it is not a throughput or HDF5 correctness test.

MIB_REPRO_STORAGE_PROBE=1 adds a bounded actual Hdf5Service write/flush/close/
reopen/readback diagnostic: 32 random 512x96 Mono8 originals and matching masks,
3,145,728 image-payload bytes. Every original and mask matched byte-for-byte on
readback. Open+append+flush+close took 0.00460932 seconds on this host in this
run. This tiny cache-assisted test is NOT sustained disk bandwidth, power-loss
durability, a RAM-capacity test, or validation of the user's recording drive.
No disk filling, global cache dropping or hardware changes were performed.

Evidence: data/buffer-repro/preflight-{unsafe-threshold,oversized-payload,
oversized-series,control}.log. Production readiness implementation unchanged.

Additional CTest result: e2e.experiment_coordinator passes, but
backend.experiment_readiness FAILS six expectations in its background-calibration
success case (lines 550–561): completion, accepted count, generation, identity,
mean image, and stability after completion. This test source and production code
were not modified. Root cause/flakiness not yet determined; do not describe the
readiness suite as passing. Full output: build/repro/Testing/Temporary/LastTest.log.

## GitHub tracking

Filed consolidated actionable findings, acceptance criteria and uncertainty boundaries:
https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/403
Related existing reliability issues: #369, #370, #367, #371.

## Implementation continuation — issue 403

Branch `fix/403-recording-safety` contains regression-first fixes after the
historical unchanged-production observations above. New asserted coordinator
regressions cover byte-pressure flushing, partial-series Stop with continuing
input, and Stop with no further input. Queue tests cover post-stop rejection
and ten concurrent producer/Stop rounds with accepted==written accounting.

Readiness rejects oversized payloads and invalidates budget/threshold changes.
A threshold above byte capacity now warns and uses automatic byte-pressure
flushing rather than starving. Destination roundtrip is cached (30 seconds or
configuration-generation invalidation), and explicitly marks sustained throughput
unverified. Capacity-for-duration and sustained throughput certification are not
claimed. Preview frame skipping is disabled during background calibration; the
success test supplies a burst, not arbitrary sleep-dependent pacing.

Observed operator symptom added to GitHub: LED visibly blinks during the original
incident. Which LED and causal connection remain unconfirmed; no electrical
timing claim or hardware fix is made without oscilloscope verification.

Validation: original queue/post-stop and partial-series assertions failed before
fixing; readiness payload/generation and destination-roundtrip assertions also
failed before their implementations. Six focused Release tests passed. TSan
requires process-local ASLR disable on this host (setarch x86_64 -R), plus the
repository's existing third-party suppression file for GDAL/TBB/GLib. No global
host setting or new suppression was added. Final sanitizer results follow below.

TSan final focused run: all six tests passed using the existing suppression
file and process-local ASLR disable. Unsuppressed warnings were confined to the
existing GDAL decoder lock-order suppression. Added explicit partial-series
restart and oversized-series readiness coverage before publishing.

Final restart and oversized-series readiness checks also pass under TSan (2/2).
Together with the prior six-test run this covers seven distinct focused tests.

Adjacent Release checks also pass (4/4): recording.experiment_roundtrip,
integration.e2e_live_view_latency, integration.e2e_realtime_throughput and
processing.realtime_drop_frames_default.
