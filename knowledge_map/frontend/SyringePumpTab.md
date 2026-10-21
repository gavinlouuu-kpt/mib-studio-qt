# SyringePumpTab

> UI for [[../services/SyringePumpService]]: dual-pump connect, flow rate,
> direction, start/stop/purge, live status.

**Source:** `src/frontend/tabs/SyringePumpTab.cpp`,
`include/frontend/tabs/SyringePumpTab.h`
**Related:** [[../services/SyringePumpService]],
[[Dialogs]] (`SyringePumpSettingsDialog`)

## Responsibility

- Per-pump (`Sample`, `Sheath`) controls: connect button, flow-rate
  spinbox + unit combo, direction, start/stop, purge, syringe volume.
- Poll timer calls `pollStatus(PumpId)` and refreshes: run status, current
  flow rate, accumulated volume, stall indicator.
- Delegate COM port / baud / Modbus address configuration to
  `SyringePumpSettingsDialog`.
- `SyringePumpSettingsDialog` now includes per-pump baud/address fields and
  in-dialog address scan; tab connect buttons consume those persisted values.

## Renumbered adapters (2026-10-21)

Both connect buttons go through `connectPump()`. After a successful
connect, `rememberPumpAdapter()` writes the adapter's identity to
config.json (`pump_sample_adapter` / `pump_sheath_adapter`:
`{vid, pid, serial, description}`) and, if it moved, the new COM number.
Before the next connect, `resolveAdapterPort()`
([[../services/SerialPortClaims]], `SerialAdapterIdentity.h`):
- follows an adapter with a serial number to its new COM port;
- refuses a configured port that now carries another adapter, so pump
  frames never reach another device;
- never guesses an adapter without a serial number (CH340): if its port is
  gone, the message names the ports with the same adapter type.

`SyringePumpSettingsDialog` labels each COM port with its adapter
description, and drops the remembered adapter when the operator picks a
different port. Configs without `pump_*_adapter` connect as before.

## Gotchas

- Sample and Sheath pumps must be on **different** COM ports; the settings
  dialog uses `SyringePumpService::getComPort(PumpId)` to enforce.
- `flowRateUnit` is an integer code, not a string — `100` = µL/min.
- See `docs/dLSP_pump.pdf` for the protocol.
