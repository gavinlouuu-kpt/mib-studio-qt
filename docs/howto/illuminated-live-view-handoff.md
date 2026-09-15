# Illuminated Live View — cross-system handoff

## Checkout

Repository: https://github.com/gavinlouuu-kpt/mib-studio-qt
Branch: `feat/one-click-illuminated-live` (base: `develop`)
PR: https://github.com/gavinlouuu-kpt/mib-studio-qt/pull/414
Issue: https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/413
Implementation through commit `4d6169b69520797add78a130db3545d87ca77824`; review
fixes from the rig PC (discovery strictness, Save-time timing validation,
cancellation, shutdown record, migration rule) follow it on the same branch —
see the branch log and the "second pass" sections of the operator guide.

```sh
git fetch origin
git switch --track origin/feat/one-click-illuminated-live
```

Use a clean checkout/worktree and read repository AGENTS.md first. This branch
is unmerged; do not assume the installed application includes it.

## User goal and delivered scope

Default XGC/R5D rig: open application and click Start Live View. No manual
setup, separate camera Apply, serial Connect, or generator Start required.

- Coordinated backend camera/strobe/generator start and stop, including failure,
  missing-frame timeout and shutdown cleanup, readback checks and ownership.
- Saved Exposure and Requested FPS controls; frequency follows FPS, duty scales
  to preserve trigger duration. Exposure/strobe timing is not silently changed.
- Advanced setup collapsed; raw JSON has a separate disclosure.
- Default automatic generator discovery: USB adapters, configured address and
  serial settings, read-only probes, exactly one compatible response required.
- Default channel 1 is known wiring, not discovered wiring. Multiple cameras
  or generators require selection; custom setups remain supported.
- Fresh defaults and exact untouched historical defaults receive the automatic
  preset; modified/external profiles are deliberately preserved.

## Key files

- `src/backend/app/AppBackend.cpp`: saved rig validation and factory wiring.
- `src/backend/camera/mindvision/MindVisionCamera.cpp`: lifecycle coordination.
- `src/backend/camera/mindvision/MindVisionApply.cpp`: checked SDK settings.
- `src/backend/services/PulseGeneratorService.cpp`: ownership and discovery.
- `src/frontend/tabs/ConfigTabs.cpp`: controls, persistence and default migration.
- `resources/defaults/mindvisionConfig.json`: automatic XGC/R5D preset.
- `tests/backend/illuminated_live_test.cpp`: fake SDK/Modbus regressions.
- `tests/frontend/config_tabs_state_test.cpp`: real-widget default/persistence checks.

Full operator guide: [illuminated-live-view.md](illuminated-live-view.md).
Task record: [implementation and validation](../../knowledge_map/task/2026-09-14-one-click-illuminated-live.md).

## Verification already performed

Local backend and Qt desktop builds; full backend CTest run (96 considered,
zero failures, exporter soak intentionally skipped); relevant frontend settings,
layout and apply tests; documentation and screenshot-manifest checks. Earlier
lifecycle implementation passed local TSan and GitHub sanitizer checks. Check
current PR CI again: later commits must not inherit an earlier head's green claim.
No new physical hardware timing acceptance has been performed on this branch.
The September 14 second pass on the rig PC added regression tests for strict
discovery (foreign, busy and unplugged adapters), cancellation during
preparation, the shared timing rules, Save refusal of an unfit FPS and the
default-migration rule, but compiled nothing locally; rely on PR CI.

Build using repository presets and SDK provisioning instructions. This host used
`linux-backend-only` and `linux-system-release`, SDK enabled, Sentry disabled.
A Python environment with NumPy was needed for the backend Python checks. Do not
copy host-specific `/tmp` virtualenv or SDK paths to a different system.

## Rig PC findings (September 14, second pass)

Host: Windows 11, MindVision SDK 2.1.10 runtime installed, installed MIB Studio
Qt (pre-#413 build) was running during the first probe. The PC had no CMake,
MSVC, Conan, Qt or Python on September 14; they were installed on September 15
(see below), so the branch now builds and tests locally. `build-windows.yml`
still only runs on `develop` pushes or a manual dispatch that cuts a
beta/release. Findings:

- SDK enumeration (read-only, no CameraInit) lists exactly one camera:
  `MV-XG51GM`, GigE at 169.254.34.249, S/N 056082722114 — single-camera
  auto-selection applies.
- Five USB serial ports: a CH344 four-port adapter (COM3–COM6) and a CH340
  (COM7). The installed app's saved generator port is COM6 (CH344 port A).
- Read-only FC03 identity probe at address 1, 9600 8N1: COM4 answered with
  twelve zero registers and behaves identically to the generator on every
  other probe (a second, never-configured module) — the lenient rule accepted
  it; adoption is now keyed on the requested channel being set. COM6 (the
  generator) reads ch1 5000 Hz / 50 %, ch2–4 0 Hz once the installed app is
  closed; the rest were silent.
- September 15: toolchain installed on the rig PC (VS 2022 Build Tools 17.14,
  CMake 4.4, Ninja, Conan 2.32 with profile `ci` plus
  `[replace_requires] cpuinfo/*: cpuinfo/cci.20231129` — ConanCenter drift
  that a cold CI cache will also hit); `windows-ninja` preset configured with
  `MIB_MINDVISION_SDK_ROOT=C:\Program Files (x86)\MindVision\Demo\VC++`,
  `MIB_ENABLE_HARDWARE_SDKS=OFF`, `MIB_USE_SENTRY=OFF`. Full build and the
  fast lane (102 tests) pass, including the Qt frontend tests.
- The default profile on this PC (`%LOCALAPPDATA%\MIB_Studio_Qt\include\
  mindvisionConfig.json`) is byte-equivalent to the historical bundled profile
  and will be upgraded to the automatic preset by this build.

## Next system's work

**Current operator instruction (September 15):** no Rigol is connected.
Use webcam visual on/off confirmation for this acceptance run instead of
waiting for scope access. Keep the LED strobe at 100 µs (the operator reports
reliable triggering needs upwards of roughly 45 µs), at 1000 fps. The scope
procedure below remains a future timing-characterization procedure, not the
current visual acceptance gate. Webcam evidence cannot establish pulse width.

1. Confirm PR CI is green on the latest head (backend build/test, ASan/UBSan,
   TSan, docs). On the rig PC: `cmake --build --preset windows-ninja-build`
   then `ctest --preset windows-ninja-test`; set `VSLANG=1033` in the build
   environment (see the build notes) or header edits will not recompile.
2. Current visual acceptance is complete on the branch desktop: automatic camera
   selection stays Idle; Play acquires about 998?1000 fps and lights the LED;
   Stop and app-close make the webcam view dark. The adjusted 2500 Hz harness
   also passed, with 100 ?s strobe unchanged. See the task record for artifacts.
3. Frames are saturated (mean 255/255) at the operator's 100 ?s exposure/strobe.
   Optical commissioning requires driver-current adjustment or attenuation.
   Preserve the operator's 100 ?s LED trigger; shortening it is not a remedy.
4. Review and merge PR #414, deploy the matching build, and verify the installed
   app on the rig. Completion of issue #413 is the operator's active instruction.
5. Nanopositioner follow-up: Coremor is now independent of EGrabber at build
   time. The attached controller is OEABT; its exact model and protocol remain
   pending. Do not claim OEABT discovery or control from the Coremor driver.

## Hardware acceptance and limits

Generator CH1 -> camera trigger; physical camera OUT1 (SDK output 0) -> R5D.
The SDK strobe polarity is 1 = active high, 0 = active low; on this rig the
image followed the strobe only with polarity 0 (September 15). That does not
establish the GPIO off level: subsequent webcam checks proved GPIO high leaves
the LED lit and GPIO low makes it dark. Stop therefore drives GPIO low regardless
of capture strobe polarity. Rigol CH2 measures trigger, CH1 LED
current. Default: ROI 512x96, exposure setting 100 us, manual strobe
100 us / delay 0 with polarity 0 (SDK active-low, see above), generator 1000 Hz / 2% (20 us trigger
pulse), rising-edge trigger. Changed on September 15 from high-level trigger,
polarity 1, 5000 Hz / 10% after rig measurements (see the operator guide).

September 10 bench result was 5 kHz LED current with 64.6 us pulses; settled
image mean about 150/255 versus 5.6 LED-off. This is prior bench evidence, not
validation of this build. Sensor exposure edges remain unmeasured; the old
47 us exposure-start inference is superseded. Generator range 400–40000 Hz is
not a camera FPS guarantee. Webcam acceptance establishes visible on/off only; precise electrical timing
remains unmeasured without an oscilloscope.

The earlier screenshot workflow's mandatory setup step is superseded by the
latest default automation. No generated screenshot is a hardware test result.
