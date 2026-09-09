Title: Bounded monitoring snapshots + sorter trigger contracts (BE-5, issue #275)

Context:
- Exposes the Monitoring data path and the sorter trigger through the bridge
  (epic #246, breakdown #266–#279). Bridge schema **v6** (additive).

Implementation Notes:
- **ProcessingService** — monitoring accumulation already existed (visibility
  gate `setMonitoringActive`, 1000-frame ring buffers). Added appended-total
  counters (`getMonitoringValidAppended`/`getMonitoringInvalidAppended`) so
  consumers can compute ring evictions (appended − held); `clearMonitoringFrames`
  resets them under the same lock (atomic clear); `isMonitoringActive` and a
  capacity getter.
- **TriggerService** — added `manualPulse()` (synthetic target-group signal),
  a periodic-test generator thread (`startPeriodicTest(intervalMs)` /
  `stopPeriodicTest`, idempotent, stopped automatically by `stop()`), and
  `hasCamera()`. Uses the same lost-notify guard as the trigger loop.
- **MockCamera** — trigger-output emulation (`setTriggerOutput` latches state,
  counts pulses, returns true) so the trigger chain is headless-testable.
- **Facade** — `MonitoringCommand{Enable,Disable,Clear}` (command type 7),
  `TriggerCommand{SetPulseDuration,ManualPulse,StartPeriodicTest,
  StopPeriodicTest}` (type 8, structured errors for invalid params / missing
  camera), `fetchMonitoringSnapshot(maxRows)` (bounded, metrics-only
  `MonitoringObjectRow`s — stable `(frameIndex, objectId)` identity, never
  image payloads), `fetchTriggerStatus`.
- **Bridge/Tauri/TS** — `monitoring_set_active/clear`,
  `fetch_monitoring_snapshot(max_rows)`, `trigger_set_pulse_duration/
  manual_pulse/periodic_start/periodic_stop`, `fetch_trigger_status`.
  The shell's Monitoring view is visibility-gated (enable on show, disable on
  hide), wires Clear Buffer / Sort Trigger / pulse duration / Periodic Test,
  and shows the bounded metric rows (chart rendering is UI-3 #268).

Verification:
- `mib-bridge cargo test monitoring_and_trigger_contract` — failure behavior
  without a camera, structured invalid-param errors, enable/clear/disable
  round-trip, bounded snapshot, pulse-duration round-trip, manual pulse count,
  periodic test start/growth/stop — all headless with the mock camera.
- Full backend CTest lane, desktop cargo tests, `npm run build`,
  `gen_bridge_contract.py --check`, `check_docs.py` green.

Follow-ups:
- Chart rendering + Tune Params editing land with UI-3 (#268) / BE-3 (#273).
- Incremental delta API can be added additively if snapshot polling proves
  too heavy at production rates (rows are bounded and image-free, so pulls
  are cheap today).
