# NanopositionerTab

> UI for [[../services/AutofocusService]]: COM port selection, manual
> voltage nudging, autofocus enable, status display.

**Source:** `src/frontend/tabs/NanopositionerTab.cpp`,
`include/frontend/tabs/NanopositionerTab.h`
**Related:** [[../services/AutofocusService]]

## Responsibility

- COM port enumeration + `probeComPort` for health checks.
- Connect/disconnect; display connection status.
- Manual voltage control (buttons drive `increaseVoltage`/`decreaseVoltage`).
- Autofocus toggle (`setEnabled`).
- Live display of running average / median ring ratio and last update
  timestamp.
- Config editor for the `AutofocusService::Config` struct (focus setpoint,
  range, step sizes, direction).

## Gotchas

- This tab moved into the Config area recently — see task
  `knowledge_map/task/2025-11-19-nanopositioner-tab.md`.
- Disconnecting applies `safeShutdownVoltage` before closing the port.


## Vendor-aware discovery (2026-09-15)

The tab displays the backend vendor inventory: CoreMorrow/XMT identification and
OEABT protocol support pending. Refresh emits `discoveryRequested`, wired by
DeviceInitManager to the existing background discovery path. During a scan,
Connect, Refresh and serial settings are disabled to prevent a competing open
or changing the settings before the result connects. No match remains explicit;
OEABT is never reported connected merely from a USB adapter identity.

The coordinator scans saved-port-first on its worker, without the old blocking
saved-port shortcut on the UI thread. It auto-connects only one protocol-confirmed
match. `frontend.config_tabs_state` verifies the inventory, Refresh signal and
scan control exclusion. The protocol implementation will arrive separately.
