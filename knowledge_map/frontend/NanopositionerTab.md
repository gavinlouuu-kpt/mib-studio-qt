# NanopositionerTab

> UI for [[../services/AutofocusService]]: backend/serial endpoint selection,
> manual voltage nudging, autofocus enable, and status display.

**Source:** `src/frontend/tabs/NanopositionerTab.cpp`,
`include/frontend/tabs/NanopositionerTab.h`
**Related:** [[../services/AutofocusService]]

## Responsibility

- Backend selection (`Auto`, `OEABT`, `CoreMOR`) and persistent serial endpoint
  enumeration. A CH341 candidate is not shown as connected until protocol
  identity succeeds.
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
- Connecting is observe-only. Disconnecting applies `safeShutdownVoltage`
  only after an active control session; a read-only session closes untouched.
- `autofocus_backend` and `autofocus_endpoint` are the canonical persisted
  selection. Legacy `autofocus_com_port` migrates to CoreMOR.
