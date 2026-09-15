# 2026-09-14 — One-click illuminated Live View

Issue: [#413](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/413)
Implementation: [PR #414](https://github.com/gavinlouuu-kpt/mib-studio-qt/pull/414)

## Request and result

Replace repeated camera Apply, capture Play and generator Connect/Start with
one saved rig setup and coordinated Play/Stop. Implemented opt-in XGC/R5D
preset, capture-owned generator/strobe sequencing, readback gating, manual
control exclusion, shutdown failure reporting and a three-second frame-stall
fault. Exposure remains visible and advanced setup is collapsed.

All changes are isolated from the dirty shared checkout. No hardware was
started, routing changed or deployed application replaced during this task.

## Evidence

Baseline regression reproduced missing generator coordination. Backend/full
Qt builds, software regression suite, Qt setup/persistence tests and TSan
checks passed locally. Hardware validation is not inferred from those tests.
The historical September 10 5 kHz/64.6 µs current result supports the initial
preset, not new-build physical acceptance or exact exposure edges.

## Next release step

PR review/CI and deployment, then Rigol commissioning of the changed build.
Daily Play/Stop does not require repeating commissioning. See the
[operator guide](../../docs/howto/illuminated-live-view.md) and
[completed execution plan](../../docs/exec-plans/completed/2026-09-14-one-click-live.md).


### Everyday FPS adjustment

Requested FPS is visible beside Exposure for a saved illuminated rig. Stop capture,
change FPS, Save, and Play to apply it through the coordinated generator startup.
It edits `live_view.frequency_hz`, not the camera's free-running speed selector.
The generator supports 400–40000 Hz; this is not a camera throughput guarantee.
The bench-tested point is 5000 FPS at 512×96. Observe actual acquisition rate and
use Rigol for physical timing acceptance when commissioning another rate.

Changing FPS preserves the trigger's active duration by scaling saved duty with
frequency: the preset's 5000 Hz / 10% becomes 2500 Hz / 5%, retaining a requested
20 µs trigger pulse. Exposure and strobe width/delay are not silently changed.
Existing backend validation rejects exposure or strobe timing that exceeds the
new period, and invalid generator duty. Legacy/manual profiles leave FPS disabled.
The real-widget regression covers visibility, persistence, duty compensation,
unchanged exposure/strobe, and restoration after reopening.


## Second pass on the rig PC (September 14, evening)

Review fixes: strict generator identity for automatic discovery (a zeroed
Modbus slave on the rig PC's COM4 was a false match), discovery errors that
name busy and non-generator ports, `live_view` parsing/timing validation moved
into `parseConfig` and applied at Save, extra cancellation checkpoints before
handle open and CameraPlay, sticky shutdown-failure record, arm-readback
diagnostics with exposure quantization tolerance, verbatim historical-default
migration rule with unit test, FPS spin commit-on-enter and duty rounding.
Regression tests: `backend.illuminated_live` (foreign/busy/unplugged adapters,
cancellation, timing rules) and `frontend.config_tabs_state` (refused FPS,
migration rule).

Rig PC state: installed app running and holding COM6 (generator, 5000 Hz per
saved settings); COM4 answers address 1 with zeros; camera not attached; no
CMake/MSVC/Conan/Qt/Python on the PC, so this build was not compiled or run
there. CI is the compile/test oracle for this pass. Hardware acceptance
(Rigol CH2 trigger, CH1 LED current at 5000 and an adjusted FPS, Start/Stop/
restart) remains outstanding and requires a deployed build.

## September 15: rig PC build and corrected discovery rule

Toolchain installed on the rig PC; `windows-ninja` build and the fast lane
(102 tests) pass, including the Qt frontend tests. A second read-only probe
with the installed app closed showed the real generator on COM6 keeps 0 Hz on
channels 2–4 (channel 1 = 5000 Hz / 50 %) and that COM4 is a second,
never-configured module answering all zeros and behaving identically on every
other register. The "all four channels configured" rule from the first cut
would therefore have rejected the real generator; adoption is now keyed on the
requested channel being set, plus a read-only syringe-volume register check
that excludes a dLSP pump left channel-enabled. `backend.illuminated_live`
carries the rig register images as regressions.

## September 15: rig measurements change the preset

`hardware.illuminated_live` (new, LABEL hardware, gated by
`MIB_TEST_ILLUMINATED_LIVE`) drives the full AppBackend path on the rig.
Results at 512×96, exposure 100 µs, 20 µs trigger pulse, host frame counts over
4–5 s, generator readback after every Stop = duty 0:

| Trigger signal type | Generator | Frames/s | Latest frame mean grey |
|---|---|---|---|
| 2 (high level, shipped preset) | 5000 Hz | 4556–4592 | 255 |
| 2 (high level) | 2500 Hz | 4497–4572 | 255 |
| 2 (high level) | 1000 Hz | 4525 | 255 |
| 0 (rising edge) | 1000 Hz | 997.3–998.0 | 255 (polarity 1, any strobe width/delay) |
| 0 (rising edge), polarity 0, strobe delay 0 | 1000 Hz | 997.5 | 255 |
| 0 (rising edge), polarity 0, strobe delay 900 µs | 1000 Hz | 997.4 | 52.2 |

High-level trigger free-runs near the camera's readout limit regardless of the
generator; rising edge gives one frame per pulse. With polarity 1 the image
never responded to strobe width (1 µs) or delay (900 µs), with polarity 0 it
went dark when the strobe could not overlap the exposure: on this rig
polarity 0 pulses OUT1 with the strobe and polarity 1 leaves it idling high.
The bundled preset, preset button and parser defaults now use signal type 0,
polarity 0, 1000 Hz / 2 % (the user's chosen operating point). Saturation at
255 even for a 20 µs strobe is an optical/R5D current matter, not software.
Rigol confirmation of the LED current waveform remains the acceptance step.

## September 15: Stop-level fix verified at 1000 fps

**Superseded inference:** the software-only result below used an incorrect
relationship between strobe polarity and GPIO off level. The webcam result
in the following section corrects it; these tests did not establish darkness.

Stop requests the inactive GPIO level for the saved strobe polarity. The
two-polarity regression was compiled against the old low-only call and failed
its inactive-level assertion; restoring the fix passed `backend.illuminated_live`.
The complete Windows desktop rebuild succeeded. All 102 fast-lane tests passed
across the initial run and an unrestricted rerun of 12 sandbox failures.
Documentation and screenshot checks passed.

Three fresh five-second hardware cycles using the bundled 1000 Hz / 2% profile
delivered 998.4, 1000.2 and 1000.2 frames/s. Reported transport loss and discard
counters were zero. Each Stop ended Idle without failure, released ownership,
and independently read back generator channel 1 duty zero. Latest-frame mean
remained 255/255. Runtime evidence is in
`data/logs/issue413-1000fps-verified.log`; regression evidence is in
`data/logs/issue413-regression-before.log`. These runtime logs stay local.
MLflow upload remains pending because neither required credential variable is
configured in this session.

Completion remains unproven: scope CH2 trigger and CH1 current measurements at
1000 Hz and an adjusted cadence, physical failure-cleanup acceptance, optical
LED-off comparison, latest-fix sanitizer CI, merge and deployment are still
outstanding. No connected Rigol USB device or documented network resource was
found; its connection address has been requested. The previously published
PR head `bb0ea38` has green CI, which does not cover this uncommitted fix.

## September 15: webcam commissioning corrects GPIO shutdown

The operator confirmed there is no Rigol available and explicitly substituted
webcam visual confirmation. LED triggering requires upwards of roughly 45 µs;
the normal 100 µs strobe remains unchanged at the 1000 fps target. The 20 µs
generator camera-trigger pulse is a separate signal.

Webcam `FF-Camera` showed bright blue illumination before Start and after Stop
with commit `930c5fb`, which drove GPIO high for strobe polarity 0. An explicit
SDK GPIO-low operation made the LED dark. Thus the inferred relationship was
wrong: GPIO shutdown must drive low on this rig regardless of strobe polarity.
The corrected regression was proven to fail against `930c5fb` and pass after
restoring low, then the complete desktop was rebuilt.

A fresh full AppBackend run with the corrected binary showed LED on during
capture and dark after Stop in webcam frames. It delivered 15042 frames over
15.05 seconds (999.4 fps), no reported transport loss/discards, Idle without
failure after Stop, and independent generator readback of zero duty. Local
evidence: `data/issue413-webcam/fixed-during.png`, `fixed-after.png`, and
`data/logs/issue413-webcam-fixed-cycle.log`. The webcam establishes visible
on/off behavior, not individual pulse timing. Camera images remain saturated.
Scope access is no longer the current acceptance blocker under the operator's
instruction. Latest-correction CI, merge and deployment remain outstanding.
