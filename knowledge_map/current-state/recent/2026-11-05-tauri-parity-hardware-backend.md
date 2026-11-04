## 2026-11-05 — Tauri parity, hardware backend rows (ABI 45; #868 HW-033, HW-038, HW-004/026/049, HW-023)

Backend and bridge pieces the hardware views need, additive over ABI 44. **Autofocus messages
(HW-033):** `BackendFacade` registers `AutofocusService::setStatusCallback` (as Qt's Nanopositioner
tab does), keeps the latest message and a sequence, and serves them as `status_message` /
`status_sequence` on `fetch_autofocus_status`; failures ("failed", "No nanopositioner") are also
Hardware/Autofocus `BackendError` events, the per-step `Voltage: ...` line is not. The callback holds a
shared mirror, not the facade, and detach waits out in-flight emissions, because the service calls a
copy of its callback outside its lock. **`list_serial_ports` (HW-004/026/049):** Qt's pump COM list and
pulse-generator port list in one read, the simulated rig's ports under `MIB_SERIAL_MODE=sim`, nothing
opened. **`reserve_pump_ports` (HW-023):** the shell's remembered pump ports reserved under
their own owner next to config.json's, so the startup probe skips the pump bus. **Boot service
toggles (HW-038):** the `bootDisabledServices` preference becomes `MIB_DISABLED_SERVICES` in the Tauri
`init`, before the backend initializes, unless the environment already sets it. New unmounted
components: `components/AutofocusStatusLine.tsx`, `areas/hardware/BootServiceToggles.tsx`. See
[[architecture/Rust-Bridge]] (ABI 45), [[architecture/Desktop-Shell]], [[services/AutofocusService]],
[[services/SerialPortClaims]], [[architecture/AppBackend]].
