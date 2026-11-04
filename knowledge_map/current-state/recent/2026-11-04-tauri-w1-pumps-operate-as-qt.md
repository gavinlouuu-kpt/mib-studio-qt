## 2026-11-04 — Tauri pumps operate as in Qt; Stop always works (#868, #855)

The desktop app's pump and autofocus controls now behave as the Qt app's (parity wave 1, hardware stream,
PR1). `HardwareControls.tsx` is split into `PumpControls.tsx`, `AutofocusControls.tsx` and
`HardwareConnectionFields.tsx`; the pure rules (ranges, exact-integer conversion, the command queue, the
hold-to-purge state machine) are in `pumpControlModel.ts`, the gate in `hardwareControlModel.ts`
(`operatorGate`). See [[architecture/Desktop-Shell]] and [[services/SyringePumpService]].

- **No Service mode, arm or experiment lock on routine controls** (decisions D1, D2): pump connect, rate, unit,
  direction, syringe volume, Start and Purge, and autofocus Enable and Jog, need only a ready backend and a
  connected device, also while an experiment runs. The trigger tests keep Service mode and the arm (UX-9, #313).
- **Stop always works** (D3, #855): Stop pump, the purge release and Disable autofocus are disabled only when
  the backend is not ready and bypass the command queue and the `busy` flag. Stop pump also sends a purge stop
  (the dLSP keeps its full-speed purge in its own register).
- **Live device status** (HW-015): every 500 ms, each connected pump is read from the device
  (`pump_poll_status`), then its status taken. The read takes no command lock and sets no `busy`; polls never
  overlap. The "Read device status" button is gone.
- **Rate, unit, direction** (HW-007/008/009): 0-100000, 4 decimals, pre-filled from the pump's configured values
  when it connects, applied 300 ms after the last edit (also while running); no Apply buttons.
- **Hold-to-purge** (HW-012): pointer down / Space / Enter purges in the selected direction, release, pointer
  leave or cancel, blur, hidden page, unmount or disconnect stops it; a release is never lost, even while the
  start is still in flight. The latched Purge and Stop purge button are gone.
- **Syringe volume** (HW-019, D9): 0.1-9999, 1 decimal, µL|mL, applied with Apply volume (also while running);
  sent as an exact whole number the dLSP takes (0.5 mL goes out as 500 µL) or refused with a message. The same
  rule covers the dLSP flow rate, which the backend would otherwise clamp to a whole 1-9999 in silence.
- **Connection defaults** (HW-005/006): baud 1200-921600 with the standard rates offered, default 115200; the
  Sheath pump defaults to Modbus address 2; a syringe pump connects with `pump_connect_endpoint`, so no µL/rev
  calibration is sent for it. The rpm unit (peristaltic) no longer survives a switch to Syringe, and the rpm
  conversion falls back to the calibration field when the status reports 0 µL/rev.
- Pump and autofocus panels poll their own status, so a failed read of one no longer blanks the other.
