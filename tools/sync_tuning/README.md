# MIB synchronization tuning

Reusable Python API and CLI for image-based MindVision XGC / LED calibration.
It measures raw frame intensity in Overview and experiment capture, sweeps
exposure/acquisition delay, finds a stable interval, and validates the chosen
settings in longer runs. The original profile is unchanged unless `--apply`
is supplied and every validation passes.

## Build and install

From a configured MIB Studio checkout (on Windows, use an x64 VS developer shell):

```powershell
cmake --build build-ninja --target mib_sync_capture
python -m pip install "./tools/sync_tuning[plot]"
```

The CMake target links the same backend as the app. Keep the executable beside
the app's deployed runtime DLLs in `build-ninja/Release`; build/deploy the desktop
target normally on a fresh Windows checkout. Linux backend builds place it in
their configured runtime directory. `MIB_BUILD_SYNC_TUNING=OFF` disables the tool.
Python 3.10+ is required. The package itself has no dependencies; `[plot]` adds
matplotlib for PNG reports. `pip wheel ./tools/sync_tuning` builds a reusable
Python wheel; the native sampler and its runtime dependencies are separate.

## Use

Check the plan while the app is still running:

```powershell
mib-sync-tune --profile "$env:LOCALAPPDATA/MIB_Studio_Qt/include/mindvisionConfig.json" --exposures 0.8,2 --delays 0,5,15,30,45,60,75 --dry-run
```

Close applications holding the camera/generator before measuring. Keep the
scene, illumination current and focus fixed. Use a representative stationary
background: changing specimens or ambient illumination can look like flicker.
The tool does not close/reopen apps, move focus, drive pumps, sort or record an
experiment. The camera and LED are exercised by the calibration sessions.

```powershell
mib-sync-tune --profile "$env:LOCALAPPDATA/MIB_Studio_Qt/include/mindvisionConfig.json" --capture ./build-ninja/Release/mib_sync_capture.exe --exposures 0.8,2 --delays 0,5,15,30,45,60,75 --output ./data/sync-calibration-01
```

Add `--apply` to that command to save validated settings automatically. Each run
requires a new output directory. Otherwise it writes `recommended.json` for
review without changing the source file. Reopen the app to load applied settings
and recapture its background. If reopening resets manual focus on your rig,
restore the recorded focus position before continuing your experiment.

Default measurement is 5 seconds **after** 1.5 seconds of settling per mode and
candidate; validation is 20 seconds per mode. `--sample-seconds`,
`--settle-seconds`, `--validation-seconds`, `--camera-index` and
`--max-cv-percent` are configurable. Exposures default to the saved exposure
only. Supply lower exposures explicitly when improved timing saturates images;
0.8 µs is the XGC sensor minimum, not a universal camera minimum.

The CLI preserves frame rate, trigger duty, LED pulse width/delay/polarity,
ROI, gain, and all unknown JSON fields. It changes only `exposure_time_us` and
`acq_trigger_delay_us`. It never assumes that the previous rig's 60 µs delay
is correct for another setup.

## Callable API

```python
from pathlib import Path
from mib_sync_tuning import Policy, tune

report = tune(
    profile=Path("mindvisionConfig.json"),
    capture_executable=Path("build-ninja/Release/mib_sync_capture.exe"),
    output=Path("data/calibration-01"),
    exposures=(0.8, 2.0),
    delays=(0, 5, 15, 30, 45, 60, 75),
    policy=Policy(max_cv_percent=1.0),
    apply=False,
)
print(report["selected"])
```

`analyze_capture(directory, policy)` re-analyzes an existing sampler output
without hardware. `select_candidate(candidates, policy)` is a pure selection
function. A custom integration can supply a `sampler` object exposing
`validate(profile_path)` and
`capture(profile_path, mode, new_output_directory, seconds, settle_seconds)`.
The data contract is the native sampler's versioned `capture.json` and
`frames.csv`; both modes are mandatory.

The native sampler can also be run independently:

```powershell
./build-ninja/Release/mib_sync_capture.exe --profile profile.json --validate
./build-ninja/Release/mib_sync_capture.exe --profile profile.json --output data/single-measurement --mode experiment --seconds 5 --settle 1.5
```

## Acceptance and reports

Measurement uses every fourth column and second row in the central half-height
of the saved experiment ROI, with the same sensor coordinates in Overview.
Saved horizontal/vertical flips are accounted for when locating that region.
Padded row bytes are excluded. The sampler retains consecutive frame identities,
host/device timestamps, frame means, spatial deviation, clipping/dark fractions,
and three horizontal-band means; it saves first/dimmest/brightest full images.

Default gates require at least 200 measured frames, 90% of requested duration,
per-frame mean 20–235 /255, at most 0.1% sampled pixels >=250 in any frame,
at most 1% reader misses, and at most 1% temporal CV in both frame means and
each horizontal band. Known transport loss/discards reject a candidate;
unsupported telemetry is reported as unknown, not zero loss. Capture FPS must
stay within 90% of the baseline in each mode. The requested rate is retained,
but this does not guarantee the camera can attain it.

Among acceptable points, selection finds runs of at least three adjacent tested
delays within `max(best_score * 1.5, best_score + 0.15 percentage points)` of the
best worst-mode/band CV, then selects an actual tested point nearest the middle.
Rejected points split intervals. Long validation must pass every gate and must
not materially worsen the short-run score or an already acceptable baseline.
No qualifying interval means no recommendation; refine the sweep or adjust
illumination rather than accepting a lucky low-CV sample.

Outputs include:

- `original.json`: exact source bytes, saved before hardware access.
- `plan.json`, candidate profiles and per-session logs.
- Per-session `capture.json`, `frames.csv`, first/dimmest/brightest PGM images.
- `report.json`: all candidates, acceptance/rejection reasons and validation.
- `recommended.json`, `summary.csv`, `REPORT.md`, optional brightness PNG.

`--apply` refuses to overwrite a profile changed during measurement, verifies
the original backup, and uses a flushed temporary file plus atomic replacement.
Cancellation requests cooperative shutdown, then stops the sweep. If a native
driver hangs past the cancellation deadline, the child is terminated and OFF
is explicitly **unconfirmed**; inspect the rig before another capture. There is
no new hardware interlock or physical current measurement here.

This calibration proves image-intensity stability under the measured scene.
It does not measure exposure edges or LED current; use the oscilloscope for
electrical timing acceptance. Detailed bench history is in the illuminated
Live View guide.

## Verification

```powershell
python tests/tools/test_sync_tuning.py
cmake --build build-ninja --target mib_backend_tests mib_sync_capture
ctest --test-dir build-ninja -R "tools.sync_" --output-on-failure
```

Hardware runs are explicit CLI invocations, never part of default CTest.
