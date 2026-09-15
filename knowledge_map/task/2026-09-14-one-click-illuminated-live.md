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

## Desktop acceptance follow-up

The real desktop exposed paths the environment-configured hardware harness
did not cover: with EGrabber disabled, implicit mock fallback blocked automatic
MindVision discovery; after selection, Overview navigation automatically
started capture. Added failing-first regressions in
`backend.mindvision_selection_state` and `frontend.run_status_ui`, then fixed
implicit selection state and the illuminated Overview gate. Both pass.
The 102-test Windows fast lane passed before the final navigation correction;
the changed frontend test passed afterward.

The local development profile in `build-ninja/include/mindvisionConfig.json`
still has the earlier 5000 Hz high-level preset and must be updated explicitly
to the requested 1000 Hz / 100 µs strobe before desktop acceptance. A Windows
Security network-access dialog currently blocks UI Stop and restart; the user
has been asked to dismiss it. No security setting was changed by the agent.

### Desktop acceptance completed after the dialog was dismissed

Rebuilt `ba149f6`, backed up the earlier development profile locally, and
installed the bundled 1000 Hz / 2% profile at the development configuration
path. Verified the exact process executable under `build-ninja/Release`.
The app auto-selected MindVision and opened Overview idle. One Play click
discovered COM6 and armed rising-edge trigger, polarity 0, width 100 µs,
delay 0 and exposure 100 µs; desktop status showed 998–999 fps. The Windows
Camera webcam preview showed the LED lit. Stop returned the command controls
to Idle and the webcam showed darkness. A second Play rediscovered COM6 and
armed the same settings. Closing the running app stopped capture generation 2,
released COM6, exited the process, and left the LED visibly dark.
Local log: `data/logs/issue413-desktop-acceptance.log` (10:07–10:08 on the rig).
The earlier dialog and development-profile blockers are resolved. This is
visual commissioning, not an electrical pulse-width measurement. Latest CI
and merge/installed-release deployment remain outstanding.

## Nanopositioner follow-up (2026-09-15)

Operator identified the connected vendor as OEABT. Existing discovery only speaks
Coremor XMT; USB adapters do not identify the attached controller. Exact OEABT
model/protocol is pending before implementing its driver. Separately fixed the
Windows build coupling that selected the autofocus stub whenever EGrabber was
disabled. Configure regression failed before the fix; desktop build and all
102 Windows tests passed afterward. Docs and screenshot checks passed. No
OEABT auto-connect or hardware motion has been claimed or verified.

## Discovery foundation while OEABT protocol is supplied separately

The operator requested proceeding with discovery now and will push the protocol
and related work later. Added a vendor registry, retained serial candidate metadata,
read-only probe injection, unique-match selection and explicit OEABT pending status.
DeviceInitManager uses the registry on its existing worker; saved ports no longer
bypass the all-port ambiguity check. Refresh requests discovery, with connection
and serial settings disabled during the scan. No OEABT protocol commands, motion,
or successful OEABT connection are claimed.

Verification: desktop build passes. Full Windows run passed 102 of 103 tests;
the new UI fixture initially constructed its tab before backend initialization.
After correcting fixture order, both discovery/concurrency and UI checks pass.
Thus all 103 checks pass across that run and the focused rerun. Documentation
and screenshot checks pass. Current-head CI/TSan and deployment remain pending.


### CI target repair

Windows core CI failed because the signing workflow requested the standalone
`processing_core_authenticode_test` target after it had been bundled into the
shared runner. Reproduced locally as an unknown target; restored its standalone
classification. The target now builds, and its real unsigned/signed/wrong-signer/
tampered-signature checks pass against a copied Microsoft-signed SDK signtool
fixture. No trust store or processing-core implementation was changed.


## OEABT protocol integration and physical discovery (September 15)

Integrated PR #416 head c947b40 (GitHub still reported it open at integration).
Resolved overlapping build/UI/discovery changes using its native endpoint model,
with the vendor registry, worker scan, unique-match requirement and Refresh
control exclusion retained. A regression proved legacy COM-only profiles forced
Coremor; they now preserve port preference with Auto vendor selection. Explicit
vendor choices remain unchanged.

Read-only CLI identified `Oeabt pzt controller` on COM7 (WCH 1a86:7523), reporting
approximately 30.8?30.9 V, maximum 100 V and PWM capability. Desktop startup
selected the camera without starting it and auto-connected OEABT on COM7.
Play discovered the generator on COM6 and ran at 999 fps; webcam showed LED on.
App-close released COM6 and the nanopositioner, and webcam showed LED dark.
No voltage or mode commands were sent; these checks do not establish voltage
accuracy or displacement. Runtime log: `data/logs/issue413-oeabt-desktop.log`.
Full Windows suite: 105/105 passed. Docs and screenshot checks passed.
