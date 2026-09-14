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
Qt (pre-#413 build) running. No CMake, MSVC, Conan, Qt or Python: this branch
cannot be compiled or run on the rig PC as it stands; CI is the compile and
test oracle, and `build-windows.yml` only runs on `develop` pushes or a manual
dispatch that cuts a beta/release. Findings:

- SDK enumeration (read-only, no CameraInit) lists exactly one camera:
  `MV-XG51GM`, GigE at 169.254.34.249, S/N 056082722114 — single-camera
  auto-selection applies.
- Five USB serial ports: a CH344 four-port adapter (COM3–COM6) and a CH340
  (COM7). The installed app's saved generator port is COM6 (CH344 port A).
- Read-only FC03 identity probe at address 1, 9600 8N1: COM4 answered with
  twelve zero registers (a foreign Modbus slave that the lenient rule accepted
  — fixed by strict adoption); COM6 was busy (held by the running app); the
  rest were silent.
- The default profile on this PC (`%LOCALAPPDATA%\MIB_Studio_Qt\include\
  mindvisionConfig.json`) is byte-equivalent to the historical bundled profile
  and will be upgraded to the automatic preset by this build.

## Next system's work

1. Confirm PR CI is green on the latest head (backend build/test, ASan/UBSan,
   TSan, docs). The second-pass changes were not compiled locally.
2. Produce a Windows build of this branch on a machine with the toolchain (or
   an authorized `build-windows` dispatch) and run it on the rig PC. Close the
   installed app first: it holds COM6, which makes discovery report the
   generator as busy.
3. Verify Start/Stop/restart with the automatic default: discovery must resolve
   COM6 (not COM4), the log line "pulse generator discovered on COM6" appears,
   and Stop leaves generator duty 0 and OUT1 low. Confirm the default profile
   was upgraded once and an edited profile is left alone.
4. Rigol-validate trigger (CH2) and LED current (CH1) at 5000 FPS and one
   adjusted FPS, recording waveforms and the actual acquisition rate. Return
   generator and LED off. Update issue #413 and PR #414. No merge without
   authorization.

## Hardware acceptance and limits

Generator CH1 -> camera trigger; physical camera OUT1 (SDK output 0) -> R5D.
OUT1 high energizes LED, low disables it. Rigol CH2 measures trigger, CH1 LED
current. Default: ROI 512x96, exposure setting 100 us, manual active-high strobe
100 us / delay 0, generator 5000 Hz / 10% (20 us trigger pulse).

September 10 bench result was 5 kHz LED current with 64.6 us pulses; settled
image mean about 150/255 versus 5.6 LED-off. This is prior bench evidence, not
validation of this build. Sensor exposure edges remain unmeasured; the old
47 us exposure-start inference is superseded. Generator range 400–40000 Hz is
not a camera FPS guarantee. Use oscilloscope measurements for timing acceptance;
SDK readback and frame brightness only supplement them.

The earlier screenshot workflow's mandatory setup step is superseded by the
latest default automation. No generated screenshot is a hardware test result.
