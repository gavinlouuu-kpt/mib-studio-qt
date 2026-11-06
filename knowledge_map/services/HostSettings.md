# HostSettings (the Tauri host's saved pump and boot settings)

> `<data>/host_settings.json`: the instrument host's own copy of what the Qt app keeps in `config.json`
> (pump connection, last rate/unit/direction) and in `QSettings` (boot service toggles). Loaded before the
> backend starts, so the saved pump ports are reserved against discovery probes from the first second.

**Source:** `include/backend/app/HostSettings.h`, `src/backend/app/HostSettings.cpp`
(`backend::app::HostSettingsStore`); wiring in `BackendFacade::initialize` and
`BackendFacade::fetchHostSettingsJson` / `hostSettingsCommandJson`
**Tests:** `backend.host_settings` (`tests/backend/host_settings_test.cpp`); desktop `pumpSettingsModel.test.ts`,
`PumpSettingsDialog.test.tsx`, `BootServicesDialog.test.tsx`, `hostSettingsStore.test.ts`
**Related:** [[SerialPortClaims]], [[SyringePumpService]], [[SimulatedSerial]],
[[../architecture/AppBackend]], [[../architecture/Rust-Bridge]] (ABI next (#890, set at merge)),
[[../architecture/Desktop-Shell]], [[../frontend/SyringePumpTab]] (the Qt original)

## Why

The Tauri host has no live `config.json`: profiles and methods are read-only, hash-checked documents, and
`setLastConfigJson` is overwritten by every profile, method or processing apply. The Qt behaviours that
need a place to live are therefore kept here, under the **same key names Qt uses**:

| Qt | Key here | Used for |
|---|---|---|
| `pump_{sample,sheath}_com_port` | same (int 1-65535) | the port as `COM<n>`; older Qt files |
| (system names, #419) | `pump_{sample,sheath}_port` (text, 512 chars) | a port by system name (`/dev/ttyUSB0`); wins over the number |
| `_baud_rate`, `_address`, `_syringe_vol`, `_syringe_unit` | same | the dialog's connection fields; Connect uses them |
| `_flow_rate`, `_flow_unit`, `_direction` | same | restored into the pump panel's fields on start (never sent) |
| `Startup/DisabledServices` | `boot_disabled_services` (CSV) | the services left off at the next start |

Ranges (a bad value refuses the whole patch, naming the key): baud 1-4000000, address 1-247, syringe
volume 0.1-9999, syringe unit 100 (uL) or 103 (mL), flow rate 0-100000, flow unit 100/103/200 (uL/min, mL/min,
rpm), direction 0/1. `null` removes a key. The file is replaced atomically (temp file + rename); a missing
file is empty settings; an unreadable or invalid file is kept out (`load_error`) and never overwritten until
the operator saves something valid.

## What it does

- **Reservation (HW-023).** `applyReservation()` hands the saved pump ports to
  [[SerialPortClaims]] `setReserved` under the owner "the syringe pumps (saved pump settings)". It is a second
  owner beside `AppBackend::setLastConfigJson`'s `pump_*_com_port`, so neither clears the other. The startup
  nanopositioner probe, the Refresh probe and the generator `auto` sweep then skip those ports, and the
  simulated ledger shows no `ForeignFrame COM3 '/1&\r'` (see the test `reservationKeepsTheProbeOffThePumpBus`).
  `clearReservation()` runs when the facade fails to initialise and after the discovery coordinator has
  shut down.
- **Boot toggles (HW-038).** `bootDisabledServicesCsv()` goes to `AppBackend::setBootDisabledServices` before
  `initialize`; a non-empty `MIB_DISABLED_SERVICES` in the environment still wins (as in Qt's `main.cpp`). Known
  tokens: `sqlite,hdf5,processing,autofocus,trigger,capture,playback,dot_grid,auto_update,all`; tokens are
  trimmed, lower-cased and `-` becomes `_`. `auto_update` is stored for parity but the Tauri app has no
  quiet updater at start, so the dialog does not offer it (a saved one is kept and shown as "also left off").
- **Scan (HW-021).** `scan_pump_addresses {port, baud, start, end, timeout_ms}` runs the new string-port
  `SyringePumpService::scanModbusAddresses` as a tracked `PumpScan` operation (addresses 1-8, 300 ms, as Qt's
  dialog). One scan at a time. The answer is read by polling `fetch_host_settings` `pump_scan`
  `{state, operation_id, port, found, error}`, because the existing event-based scan answers only an
  integer COM port and the dialog cannot read events.

## Bridge surface (ABI next (#890, set at merge))

`fetch_host_settings` -> `{valid, path, settings, load_error, boot_env, pump_scan}` (a read for every client);
`host_settings_command {action: set_pumps | set_boot_services | scan_pump_addresses, ...}` (control: refused for
viewers). Details in [[../architecture/Rust-Bridge]].

## Limits

- Saved ports are reserved even when no pump is connected; a fresh profile with nothing saved still lets the
  startup probe identify a CH340 pump dongle (the Qt behaviour; save the pump port once to keep it off).
- `host_settings.json` is per data directory, not per profile.
- The Qt `config.json` of a machine that also runs the Qt app is not read.
