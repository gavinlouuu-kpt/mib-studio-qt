# MindVision overview and experiment ROI

## Behavior

- Overview selects the full native sensor, zero offsets, and a 400 Hz external
  trigger for illuminated profiles. The generator's minimum is 400 Hz; the GUI
  refresh limit remains 50 fps. Trigger pulse duration is preserved to the
  generator's 0.01% duty resolution (20 us becomes 0.80%).
- Experiment uses the saved JSON crop and trigger rate. ROI size is editable,
  defaulting to 512x96. Drag coordinates remain sensor coordinates under image
  mirroring. Processing receives the hardware crop at local offset (0,0).
- Mode changes join capture and realtime processing, replace the frame store,
  and leave restart to the existing camera controller. Overview has eight slots;
  Experiment restores the previous capacity. Idle navigation does not start
  illumination. The experiment readiness gate rejects Overview capture.
- Atomic JSON updates preserve unrelated settings. Failed saves restore the
  visible selection. Exact SDK readback prevents silently capturing a different
  region when a requested size is unsupported.

## Verification

- Regression-first: `frontend.mindvision_overview` failed on the original code
  because the initial ROI came from eGrabber JS instead of MindVision JSON.
- New backend coverage: 30 alternating overview/experiment capture cycles,
  frame accounting, buffer cleanup, unknown capability, rejected apply, and
  mismatched geometry readback. Illuminated fault tests cover both modes.
- Frontend coverage: JSON round-trip, editable size, failed-save rollback,
  mirrored ROI coordinates, provider switching, fresh mode buffers,
  crop-local processing ROI, experiment readiness, and MainWindow navigation.
- Windows release app and shared test runners build successfully. All 115 tests
  in the broad suite passed (excluding hardware, network, and performance labels); targeted live-view
  latency checks also pass. Logs are under `data/logs/mindvision-overview-*`.
- `check_docs.py`, `check_screenshots.py`, and `git diff --check` pass.
- TSan requires the Linux `sanitizers.yml` lane; this Windows machine has no
  installed WSL distribution. It was not run locally.

## Desktop acceptance

The tested Release executable was installed on 2026-09-15. The operator
confirmed that it works well and approved committing and merging to develop.
Installed executable SHA-256:
`30E4E95EF4F64A630CF77022E9F1B906D3FF26457291456AC98C3E44003AF27E`.
The tested desktop also contains separate hardware-shutdown work; that work
is tracked on its own branch.

## Connected rig acceptance

`hw_illuminated_live_test` with `MIB_TEST_OVERVIEW_MODES=1` alternated modes for
four runs of approximately three seconds each. A temporary profile selected
512x96 at (64,48), preserving the installed experiment profile.

| Run | Mode | Actual geometry | Measured capture rate |
| --- | --- | --- | --- |
| 1 | Overview | 816x624 at (0,0) | 400.5 fps |
| 2 | Experiment | 512x96 at (64,48) | 997.4 fps |
| 3 | Overview | 816x624 at (0,0) | 400.5 fps |
| 4 | Experiment | 512x96 at (64,48) | 997.9 fps |

Generator frequency/duty readback matched each requested mode. Every run had
zero reported transport loss and an independent generator OFF readback after
stop. This establishes SDK/serial configuration and delivered frames; physical
LED timing was not remeasured with an oscilloscope.

A separate unsupported-size probe requested 513x97 at (65,49). The SDK reported
512x96 at the same offset, and startup correctly failed before generator enable.
This confirms that native size constraints must be checked through readback;
eGrabber alignment constants do not describe MindVision offsets.

Related: [[../camera/MindVisionCamera]], [[../frontend/OverviewTab]],
[[../architecture/AppBackend]], [[../architecture/Threading-Model]].

## Test coverage (moved from Build.md, 2026-09-21)

`backend.mindvision_overview_mode` exercises geometry faults and repeated capture
cycles; `frontend.mindvision_overview` covers JSON ROI persistence and mode state.
Both use existing shared test runners. `hw_illuminated_live_test` optionally
alternates full-sensor and experiment modes with `MIB_TEST_OVERVIEW_MODES=1`;
use an even number of runs to finish with the saved experiment settings.
