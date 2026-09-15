Title: Autofocus / nanopositioner control and status bridge (BE-8, issue #278)

Context:
- Exposes `AutofocusService` (Coremor XMT nanopositioner) through the bridge
  (epic #246). Bridge schema **v11** (additive). On Linux the service is the
  platform stub (`AutofocusService.stub.cpp` — Coremor SDK is Windows-only),
  so connect fails with a structured message and every command stays safe.

Implementation Notes:
- **Facade** — `AutofocusCommand{Connect, Disconnect, SetEnabled,
  IncreaseVoltage, DecreaseVoltage, SetConfig}` (command type 11) with
  structured validation (COM port, device address, voltage range) and the
  serial-port conflict rule mirrored from BE-7: connect is rejected when a
  syringe pump already uses the COM port. Disconnect disables control first
  so no motion outlives it; enable/jog require a connected controller.
  `fetchAutofocusStatus` exposes connection/enable state, live voltage, COM
  port, ring-ratio average/median, and **explicit freshness**
  (`lastRingRatioUpdateUs` + computed `ringRatioAgeUs`) so stale focus
  metrics are observable — the Windows control loop separately enforces
  `ringRatioStaleMs` before any move. `fetchAutofocusConfig`/`SetConfig`
  round-trip the full `Config` struct as plain values (no QSettings types
  anywhere).
- **Bridge/Tauri/TS** — `autofocus_connect/disconnect/set_enabled/jog/
  set_config`, `fetch_autofocus_status/config`. The shell's sidebar
  Autofocus/Nanopositioner sections now show live backend state (ring width,
  controller state, COM port, voltage, metric age); the full control panel
  lands with UI-3 (#268).

Verification:
- `mib-bridge cargo test autofocus_commands_and_config_roundtrip`: valid
  disconnected status with explicit never-updated freshness, structured
  parameter errors, safe failure without hardware, control rejected when
  disconnected, idempotent disconnect, full config round-trip, and
  invalid-voltage-range rejection leaving config intact.
- Full backend CTest lane, desktop cargo tests, `npm run build`,
  `gen_bridge_contract.py --check`, `check_docs.py` green.

Follow-ups (tracked on #278):
- Transport seam + stub positioner for a headless sweep state-machine test
  (start/progress/converge/cancel/failure) — needs the Windows
  `AutofocusService.cpp` refactored onto an injectable XMT transport.
- Real nanopositioner closed-loop acceptance on Windows.
