# Reusable synchronization calibration — 2026-09-16

The operator requested a reusable form of the image-intensity tuning workflow.
Implementation lives in `tools/sync_tuning/`: a versioned installable Python
package plus a standalone C++ sampler linking the existing AppBackend.

`mib_sync_tuning.tune()` / `mib-sync-tune` measure baseline and candidate
exposure/delay pairs in both camera modes. Acceptance checks frame count and
measurement duration, brightness/clipping, temporal CV in the ROI and three
horizontal bands, missing reader frames, available loss telemetry, and rate
relative to baseline. Three neighboring near-best tested delays are required
for an interval; the midpoint is validated in longer independent runs.

Hardware ownership/stop remains in AppBackend. The tool does not start the
frontend, focus, pumps, sorting or recording. Native exceptions/cancellation
use coordinated stop; Python timeouts request cancellation and abort the sweep.
A driver that requires force termination is explicitly not confirmed off.

Applying is optional. Original bytes are backed up; unknown keys/timing fields
are preserved; changed source files block application; temporary write/fsync
and atomic replacement protect the source against partial writes. Camera ABI,
processing plugin and production capture code are unchanged.

Tests cover padded/malformed MONO8 regions, dark/saturated streams, band flicker,
corrupt/truncated evidence, frame accounting, rate regression, plateau selection,
full fake-sampler workflow, preflight/capture/validation failures, cancellation,
concurrent profile edits and atomic-write fault injection. CTest entries are
`tools.sync_intensity`, `tools.sync_tuning`, and `tools.sync_capture_help`.
No hardware opens during these tests or `--dry-run`/`--validate`.

Local runtime artifacts and hardware evidence are under `data/sync-package/`.
The new band checks are deliberately stricter than the earlier whole-region
manual tuning; offline legacy replay must not be called independent validation
of a newly selected setting. Real long validation is required before applying.

See [package usage](../../tools/sync_tuning/README.md) and
[[../camera/MindVisionCamera]].

Validation: 22 Python contract tests and all seven focused CTest entries passed, including existing illuminated lifecycle, mode switching, frame-store stress and hardware shutdown tests. Native sampler completed ten real camera sessions (baseline, three candidate pairs, validation) with confirmed stop. The 10-second validation at the selected 60 us / 0.8 us setting rejected Overview band CV 1.004% against the 1% default plus short-run consistency limits; no source profile was modified. This demonstrates hardware acquisition and the rejection path, not a newly accepted calibration. The wheel was built and the package installed locally.
