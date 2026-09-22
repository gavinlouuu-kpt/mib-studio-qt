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

## Status callback lifetime (#405)

The registered callback owns a shared admission gate, not the tab. The tab
invalidates the gate under its mutex before deleting its UI. A callback may
outlive the tab or be copied before subscription replacement: late calls are
ignored. Admitted messages use context-bound `Qt::QueuedConnection` delivery;
widget access runs only on the GUI thread and Qt discards pending deliveries
on receiver destruction. No callback is invoked synchronously into widgets.

`frontend.nanopositioner_callback` uses the hardware-disabled service stub to
exercise the real tab: late delivery, worker-thread delivery, pending events,
50 concurrent destroy/recreate cycles, and reentrant unregistration. It does
not qualify physical disconnect, shutdown voltage, or Windows SDK behavior.
