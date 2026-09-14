# Illuminated Live View — cross-system handoff

## Checkout

Repository: https://github.com/gavinlouuu-kpt/mib-studio-qt
Branch: `feat/one-click-illuminated-live` (base: `develop`)
PR: https://github.com/gavinlouuu-kpt/mib-studio-qt/pull/414
Issue: https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/413
Implementation through commit `4d6169b69520797add78a130db3545d87ca77824`.

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

Build using repository presets and SDK provisioning instructions. This host used
`linux-backend-only` and `linux-system-release`, SDK enabled, Sentry disabled.
A Python environment with NumPy was needed for the backend Python checks. Do not
copy host-specific `/tmp` virtualenv or SDK paths to a different system.

## Next system's work

1. Review automatic discovery, identity matching, startup cancellation/failure
   cleanup and default migration; retain custom-profile protection.
2. Check current CI; build and run tests on the target platform, including its
   actual serial adapter enumeration behavior. Resolve any review findings.
3. Deploy/run this branch on the rig when authorized, without assuming a merge
   is already authorized. Verify application start/stop/restart and cleanup.
4. Rigol-validate trigger and downstream LED current at baseline and adjusted
   FPS, recording waveforms and actual acquisition rate. Update issue #413.

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
