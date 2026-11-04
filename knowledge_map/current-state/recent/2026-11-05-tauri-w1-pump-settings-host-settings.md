## 2026-11-05 — Tauri pump settings dialog, saved connection, reserved pump ports, boot toggles (#868, bridge ABI 46)

Parity wave 1, hardware stream, PR4 (stacked on PR1 #875 and PR2 #881). The desktop app gets Qt's
`SyringePumpSettingsDialog` and the pump settings that survive a restart. New host store
[[services/HostSettings]] (`<data>/host_settings.json`, Qt's key names); bridge ABI 46
(`fetch_host_settings`, `host_settings_command`, see [[architecture/Rust-Bridge]]).

- **HW-020, HW-004, HW-021.** Settings > Syringe Pump Settings… (and a "Pump settings…" button on the pump
  panel) opens `PumpSettingsDialog`: per pump an enumerated port list with Refresh (PR2's `list_serial_ports`),
  baud, Modbus address, Scan (addresses 1-8, 300 ms, Qt's messages) with a result list and Use, and the syringe
  volume. OK / Cancel / Apply; Apply is atomic (both pumps are checked, then one write), and a changed volume
  is also sent to a pump that is connected (D9: exact whole integer or refused). PR1's inline connection
  fields and Apply-volume row moved into the dialog, as in Qt.
- **HW-003.** Connect on the pump panel uses the saved port/baud/address/volume; with no saved port it says
  so (Qt's message) instead of guessing.
- **HW-016.** Rate, unit and direction are saved after every command that succeeded (and on page hide and
  Disconnect) and restored into the fields at start. Restoring sends nothing.
- **HW-023.** The saved pump ports are reserved at facade start ([[services/SerialPortClaims]] second
  owner), so the startup probe and Refresh never open the pump bus. Backend test: without saved ports the
  simulated ledger records `ForeignFrame COM3 '/1&\r'`; with them it records none and the nanopositioner
  still connects on COM5.
- **HW-037.** Startup discovery's automatic selection is on by default; an explicit opt-out (stored
  `false`) still turns it off (`StartupDiscoveryControls.tsx`, one line).
- **HW-038.** Settings > Boot Service Toggles… (`BootServicesDialog`) saves the services to leave off at the
  next start; the facade applies them before the backend starts, `MIB_DISABLED_SERVICES` wins.
- New dialogs open through window events (`hardwareDialogEvents.ts`), mounted once by `<HardwareDialogs />` in
  `areas/shell/Dialogs.tsx`; viewers get both menu items disabled and the commands are refused by the server.
