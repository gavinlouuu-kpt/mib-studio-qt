## 2026-11-05 — Tauri nanopositioner panel works as Qt's tab (#868, parity wave 1, PR3)

The desktop app's nanopositioner / autofocus panel (`AutofocusControls.tsx`) now follows Qt's `NanopositionerTab`:
Enable autofocus as a check box, the target ring width applied live (1-100, 2 decimals, default 20, remembered), Jog
with a Step of 1-100 V, a backend selector (Auto-detect | OEABT serial | CoreMOR) over a device combo fed by the
discovery job with a Refresh button, CoreMOR baud and address, runtime failure messages, and a remembered selection.
Rules in `autofocusControlModel.ts` and `autofocusMessages.ts`; see [[architecture/Desktop-Shell]] and
[[frontend/NanopositionerTab]].

- **Operator controls (D2, D3).** Enable, the setpoint and Jog need only a ready backend and a connected device (the
  setpoint only a ready backend), also during an experiment. Disable is always available when the backend is ready, and an
  Enable or Jog still queued behind another command is never sent after it.
- **Messages (HW-033).** Failures from the backend's control thread and the reason of a refused connect reach the panel
  and the event log through the existing `BackendError` event, see [[services/AutofocusService]] and
  [[architecture/Rust-Bridge]] (no ABI change).
- **Ring width (HW-034).** The sidebar dims the value past `ring_ratio_stale_ms` (read by the shell with the status; Qt
  fixed 1000 ms), the status bar carries `Ring width=`, and the Connect-stage focus gate uses the same limit.
- **Remembered values (HW-036).** The manual selection is stored in the startup preference record on connect,
  disconnect and page hide, and pre-fills the panel; the target ring width is stored on every edit and restored once
  into a fresh backend.
- **Latest wins, and Home (2026-11-07, Gavin, MIBQT-45: "Latest should win, profile setting of the nanopositioner
  doesn't make sense ... Sometimes user wants to go back to 'home' a fixed value").** The remembered target ring width
  is written after the startup profile or method restore has settled (`startupRestoreGate.ts`), never before it, so the
  restore cannot overwrite it; a profile or method applied later in the session is the newest value and is remembered.
  A **Home (N)** button returns to the value the applied profile or method set (the default 20 when none did), shows
  that number, and is refused during an experiment. Run provenance records the live value at Start as
  `piezo.focus_setpoint` (snapshot v3; files from before read it as unknown), see [[data-model/HDF5-Storage]].
- The OEABT connect now sends the baud and address fields instead of the literals -1/115200/1; the stale 'lands with
  UI-3 (#268)' sidebar tooltip is gone.
