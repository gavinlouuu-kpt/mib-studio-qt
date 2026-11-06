## 2026-11-04 — Pulse generator and trigger tests as in Qt, in the React/Tauri app (#868, Tauri parity wave 1, Hardware PR2)

The desktop app's acquisition pulse generator panel (`components/PulseGeneratorControls.tsx`, rules in
`pulseGeneratorModel.ts`, settings in `pulseGeneratorStore.ts`) now behaves like Qt's (`ConfigTabs.cpp`):
an enumerated port list with description, S/N and VID:PID and Refresh; baud 4800–115200 (default 9600), data
bits, parity, stop bits; Modbus address with a read-only **Scan** of addresses 1–16; **Set**, **Start** (frequency,
duty and output on in one click, one backend command) and **Stop** with no Service mode and no arm (D5: Connect,
Disconnect, Set and Start are locked while an experiment is active because the generator is the acquisition clock;
Stop always works, even queued behind a command in flight, and is sent as soon as that command's answer arrives); Qt's status wording ("Verified addr N — ChX: f Hz,
d% duty, output ON/off"); the port, framing, address, channel, frequency and duty survive a restart (the
instrument's shell preferences, with USB-identity re-resolution of a renamed port); and the one-click **Use XGC +
R5D preset for Live View** that writes Qt's MindVision rig preset and the generator connection into the camera
document's `live_view`. The trigger tests in the Monitoring toolbar stay in Service mode behind the arm (D4) but take
Qt's limits (pulse 1–1,000,000 µs applied live and before firing, so the log never states an unapplied duration;
periodic interval 10–60000 ms, locked while running; refused input shows a message instead of being replaced), a
running periodic test stops when the Monitoring view is hidden, and a refused Service-mode request is shown instead
of swallowed. Qt's sort-trigger counters (targets, conversion, served, lost, latency) are in the status bar.

Backend (ABI next (#881, set at merge), additive): `list_serial_ports` (read-only enumeration), the sort-trigger funnel in
`fetch_trigger_status`, the module's limits and framing in `pulse_generator_status`, and the `set` / `start`
actions of `pulse_generator_command`. See [[architecture/Desktop-Shell]], [[architecture/Rust-Bridge]],
[[services/PulseGeneratorService]] and [[services/TriggerService]].
