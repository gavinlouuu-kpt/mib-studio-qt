Title: Tauri pump and autofocus operator controls

Context:
- Closes the existing-but-unwired pump and autofocus bridge control gap during
  React migration. Uses existing typed bridge commands; no backend or Qt change.
- Relevant architecture: [[architecture/Desktop-Shell]],
  [[services/SyringePumpService]], [[services/AutofocusService]].

Implementation:
- `desktop/src/components/HardwareControls.tsx` renders both Sample and Sheath
  connection, rate, direction, run/stop, purge/stop-purge and syringe volume
  controls, plus autofocus connection, enable/disable, jog and full config.
- The host supplies `ready`, `experimentActive`, `append`, commissioning `mode`,
  `armed`, and `onDisarm`. Hardware actuation reuses `canActuate`; arm is consumed
  before invoking motion. Every panel command has synchronous ownership to
  reject overlapping clicks. Backend remains authoritative on serial conflicts.
- Experiments lock configuration and motion; stop/disable remain enabled even
  when status is unknown or commissioning is disarmed. Unknown/invalid status
  disables other mutations. Polling snapshots does not automatically actuate or
  connect devices. Explicit pump status refresh reaches the device.
- Failed commands and failed status reads are surfaced, never optimistic success.
  Config is loaded from backend, not invented defaults, and edits validate finite
  values, voltage range, step size, and sample policy. Syringe volume follows
  the device's integer 1–9999 limit; unit mappings reuse Qt's 100/103 values.

Limitations:
- Existing bridge uses numeric COM identifiers; typed serial endpoints/discovery
  remain follow-up work. Config applies to runtime only; no persistence promise.
- Pump live-rate/volume are shown as device values; configured flow units are
  explicitly labeled. No real hardware connection or actuation was performed.
- This standalone component must be mounted by the desktop integration change.

Verification:
- Added model tests for experiment/commissioning gates, numeric/config validation,
  and command ownership including failure recovery.
- Added jsdom interaction tests for non-actuating mount, explicit one-shot arm,
  experiment lock and stop escape, backend errors, invalid status, blank inputs.
- 134 frontend tests pass; TypeScript and Vite production build pass. jsdom is
  added by the integration branch (no dependency files owned by this change).
