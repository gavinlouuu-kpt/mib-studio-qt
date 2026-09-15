Title: Syringe-pump commands and status bridge (BE-7, issue #277)

Context:
- Exposes the Qt-free `SyringePumpService` (dLSP pumps over Modbus RTU via the
  `ISerialPort` seam) through the bridge for both pump identities (epic #246).
  Bridge schema **v10** (additive). Real dLSP hardware acceptance remains the
  Windows half of #277.

Implementation Notes:
- **Facade** — `PumpCommand{Connect, Disconnect, SetFlowRate, SetDirection,
  Start, Stop, Purge, StopPurge, SetSyringeVolume, PollStatus, ScanAddresses}`
  (command type 10) with structured validation (pump id, COM port, Modbus
  address 1–247, flow rate, syringe volume) and **serial-port conflict
  rules**: a connect is rejected when the other pump or the connected
  autofocus controller already uses the COM port. Disconnect stops an active
  run/purge first. `fetchPumpStatus(pumpId)` returns the authoritative
  per-pump snapshot (connection, run state, live/config rates, direction,
  stall). The address scan runs as a BE-1 tracked operation (kind `PumpScan`,
  the Completed event's text carries the comma-separated addresses) so the
  multi-second probe never blocks the bridge event queue or UI.
- **Bridge/Tauri/TS** — flat `pump_*` commands + `fetch_pump_status` +
  `pump_scan_addresses`; contract gains `pump_ids`, `pump_run_states`,
  `pump_directions`, operation kind `PumpScan` (all pinned by shim
  static_asserts and the generated TS mirror). The pump control dialogs are
  UI work tracked on UI-3/UI-5.

Verification:
- `backend.pump_bridge_facade` (CTest) — both pump identities end to end over
  the fake Modbus serial through the facade command surface: connect/min-max
  parse/control/poll/disconnect, structured parameter errors, and the
  COM-port conflict rejection.
- `mib-bridge cargo test pump_commands_fail_safely_without_hardware` —
  snapshots for both identities, structured errors without hardware, no-hang
  connect failure, and the scan completing as a tracked operation.
- Full backend CTest lane, desktop cargo tests, `npm run build`,
  `gen_bridge_contract.py --check`, `check_docs.py` green.

Follow-ups:
- Recorded dLSP hardware acceptance run on Windows (issue checkbox).
- Pump control UI (dialog parity) with UI-3.
