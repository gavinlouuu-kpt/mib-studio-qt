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


### Discovery service (#419)

Refresh only emits `discoveryRequested()`; `DeviceInitManager` runs a
nanopositioner job on [[../services/DeviceDiscoveryService]] and delivers
the result through `showDiscoveryCandidates(snapshot)`, which fills the
combo with identified **and** unidentified endpoints (identity is still
verified only when connecting). The tab no longer calls
`AutofocusService::availableEndpoints()` itself: until the first job runs the
combo is empty and the status says "Click Refresh to search for
nanopositioners." `setDiscoveryRunning(true)` is applied on the `Started`
outcome and cleared on any terminal outcome; the retry text is
"Searching for nanopositioner... (retry n/m)" as before.

### PR #413 discovery integration

The shared vendor registry now uses native nanopositioner endpoints and includes
both OEABT and CoreMorrow/XMT probes. Startup scans all candidates on its worker,
auto-connects only a unique validated match, and Refresh repeats discovery.
Connection and serial/vendor controls are disabled while scanning. A legacy
COM-only setting retains its port preference but defaults to automatic vendor
selection; an explicit saved vendor is preserved. Discovery and connection are
observe-only, with no voltage or mode writes.

## Callback lifetime and voltage persistence (#405, TD-21)

Autofocus callbacks capture a shared `StatusDelivery` gate. Queue admission and
tab destruction share its mutex; destruction clears the target before deleting
widgets, and Qt discards already queued deliveries. Unregistering the callback
alone cannot invalidate a snapshot already taken by the service.

`saveConfig()` preserves `autofocus_initial_voltage`: connecting, observing the
live voltage (including 0 V), unrelated edits and quitting do not edit that
setting. The configured initial voltage remains separate from manual nudges.
`frontend.nanopositioner_callback` uses a fake driver and retained callback
snapshots to cover both persistence and concurrent destruction.
