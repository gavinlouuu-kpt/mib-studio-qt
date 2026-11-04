# 2026-11-05 — Tauri pulse generator panel and trigger tests, Qt parity (#868)

**Why:** the hardware parity rows HW-043..057 compare the Tauri panels with the Qt tabs
(`ConfigTabs.cpp`, `ExperimentMonitoringTab.cpp`). Branch `feat/tauri-parity-hardware-pulse`.

**Decisions**
- Sort Trigger and Periodic Test keep Tauri's Service mode + one-shot arm (they actuate the sorter);
  only the Qt tooltip text is ported (HW-043).
- The pulse generator stays in the hardware panel (Connect stage, reachable from every tab once the
  panel is shared); Qt's "Advanced - Hardware Setup" location is not copied (HW-048).
- The Modbus address stays 1–247: the backend refuses 248–255, which Qt's spin allows (HW-051).
- Leaving the Monitoring tab stops a running periodic test but leaves the shared arm alone: the arm
  also covers the Connect-stage pulse generator, pumps and stage.
- The pulse duration / periodic interval bounds are frontend-only; the backend checks `>= 1`.

**Open (needs another area or the backend)**
- HW-049: no bridge command lists serial ports. Add `list_serial_ports` (read-only, over
  `serialbus::availablePorts()`: system name, description, serial number, VID, PID, location), then the
  port field becomes Qt's combo with Refresh and the saved USB identity can re-resolve a renamed port.
- HW-046: `TriggerStatus` has no target / conversion / served / dropped / latency counters. They come
  from `ProcessingService::getIdentificationCounters()`, `TriggerService::getDroppedRequestCount()` /
  `getDroppedPulseCount()` and `PipelineTimingRecorder::avgTargetLatencyUs()`.
- HW-047: Soft Trigger stays in `cameraDocument.tsx` (config area): armed Service mode, running
  MindVision camera, idle experiment. Qt has it ungated under "Advanced - Hardware Setup"
  (`ConfigTabs.cpp:361-363,1332-1338`; the backend refuses when capture is not running).
- After "Use XGC + R5D preset" the camera document editor still shows its old copy and refuses to
  apply it (revision mismatch) until it reloads the file.

See [[architecture/Desktop-Shell]].
