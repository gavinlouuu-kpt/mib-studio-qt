# One-click illuminated Live View

Status: completed — implemented and locally verified; release and physical commissioning remain separate.

Issue: https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/413

## Goal

Normal use is open app, Play, Stop. Hardware setup is a saved commissioning
task, not a recurring sequence across camera/strobe/generator panels.

## Decisions

- Reuse CaptureService, MindVisionCamera and PulseGeneratorService/SerialBus;
  no new external platform or transport is appropriate for this Qt workflow.
- Opt in through one saved camera JSON profile to preserve manual/mock rigs.
- Capture owns configuration, generator ordering and partial-failure cleanup.
- Keep tested command values separate from measured current and unmeasured
  exposure edges. No oscilloscope prerequisite for every ordinary Play.

## Verification

Baseline regression reproduced camera-only start of an illuminated profile.
New fake SDK/Modbus and Qt tests cover ownership, failures and persistence.
Backend/full desktop builds, existing regression suite and sanitizer checks
are run before publishing. Physical timing acceptance remains a separate
Rigol commissioning run; no new hardware measurements are claimed.

## Results

- Backend and full Qt desktop builds passed with the pinned MindVision SDK.
- Backend suite: 96 tests considered, 95 passed and exporter soak skipped.
  The initial Python/NumPy failure was resolved in an isolated test venv.
- Seven relevant real-widget tests passed; setup/Stop tests rerun after final UI changes.
- Final lifecycle, pipeline stress and live-view latency checks passed.
- TSan instrumented backend suite passed after using process-local `setarch
  x86_64 -R` to avoid this host's sanitizer address-layout startup failure;
  the crash/segfault probe is intentionally skipped under TSan. No host routing
  or system-wide ASLR setting was changed.
- Documentation, screenshot-manifest and diff whitespace checks passed.
- No physical hardware was started or changed during this implementation.
