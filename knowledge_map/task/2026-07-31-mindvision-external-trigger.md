# 2026-07-31 — MindVision external trigger + strobe sync + pulse generator

> Tracking note for epic #323 (sub-issues #324 camera, #325 pulse generator,
> #326 GUI, #327 verification). Branch
> `claude/mindvision-external-trigger-5d1ohk`.

## Goal

Move MindVision acquisition from free-run to triggered capture: an external
TTL pulse train (Zhongsheng 脉冲频率与占空比输出 RS485 module) triggers each
exposure, with the camera strobe synced to exposure for illumination.
Software trigger (mode 1) supported for bench testing without the pulse
source.

## What was implemented

- **Config schema** (`MindVisionConfig.h`): `ext_trig_signal_type` [0,4],
  `ext_trig_jitter_us` / `acq_trigger_delay_us` [0,1e6], `trigger_count`
  [1,1000]; `strobe_mode` clamp widened [0,2]→[0,3].
- **Shared apply helper** `MindVisionApply.{h,cpp}` —
  `applyConfigToHandle(hCamera, cfg, firstError)`; both
  `MindVisionCamera::applyJsonConfig` and
  `CameraControlService::applyMindVisionJsonToCamera` route through it
  (drift closed). New setters go right after `CameraSetTriggerMode`, before
  `CameraPlay`. No `API_LOAD_MAIN` in this TU.
- **softTrigger()**: `ICamera` default-false virtual →
  `MindVisionCamera::softTrigger` (`CameraSoftTrigger` under `stateMutex_`)
  → `CaptureService::softTriggerActiveCamera` (under `cameraMutex_`; same
  lock order as `stop()`) → `AppBackend::softTriggerCamera` → facade
  `CameraCommandAction::SoftTriggerCamera`.
- **checkDeviceHealth**: skips the frame-consuming probe when
  `configuredTriggerMode_ != 0` (probe would eat a triggered frame; timeout
  is normal idle). `CameraConnectTest` deliberately NOT used — symbol
  presence in deployed `CameraApiLoad.h` function tables is unverifiable
  from CI.
- **Bug fixes**: `grabFrame` copies `outBuffer_` under `stateMutex_`
  (use-after-free vs `stop()`); `CameraSetIspOutFormat(MONO8)` unconditional
  (color-sensor buffer overrun). Deferred: `configureTriggerOutput`
  index-before-open (dead branch).
- **PulseGeneratorService** (Modbus RTU via `ModbusRtu.h`, pump-service
  pattern): connect/verify (FC03 read, no writes), `setFrequency` (FC16,
  u32=Hz×100 high-word-first, clamp 400–40k), `setDutyCycle` (FC06,
  u16=%×100), `setOutputEnabled` (duty-0 gating — module has no run/stop
  register). Owned by `AppBackend::pulseGenerator()`.
- **GUI**: new ConfigTabs tab "MindVision config (mindvisionConfig.json)" —
  editor + Reset/Save/Apply/Soft Trigger/Browse/Clear plus a pulse-generator
  group (COM/baud/addr, channel, freq, duty, Set/Start/Stop).
- **Sample config**: `resources/defaults/mindvisionConfig.json` + qrc entry.
- **Tests**: `backend.pulse_generator_frame` (manual §2.3 known-answer
  frames), `backend.mindvision_config` cases 10–15 (new fields, clamps),
  facade boundary test soft-trigger negative cases.

## Verification status

- Linux stub build (backend + frontend): green; 44/44 backend tests pass;
  `scripts/check_docs.py` clean.
- **Linux GUI e2e (2026-07-31, Xvfb + xdotool + screenshots)**: the
  "MindVision config (mindvisionConfig.json)" tab renders in ConfigTabs with
  the full button row; the editor auto-seeds from
  `:/defaults/mindvisionConfig.json` (all trigger/strobe keys present); the
  pulse-generator row is disabled until connected. Soft Trigger click was
  traced under gdb through the intended chain
  `ConfigTabs::onSoftTrigger → AppBackend::softTriggerCamera →
  QMessageBox::warning` (capture stopped → error path). Pulse-generator
  Connect click reached `PulseGeneratorService::connect`, which attempted
  the serial open and failed cleanly ("failed to open COM1") as expected on
  Linux. Caveat: modal QMessageBox windows do not map under bare
  Xvfb/openbox in this container — this affects pre-existing dialogs (e.g.
  Apply Camera Script) identically, so it is an environment artifact, not a
  regression.
- **Windows real-SDK build: NOT yet done** (CI never compiles it). Before
  merging: build with `-DMIB_ENABLE_MINDVISION=ON`, grep the installed
  `CameraApiLoad.h` for `CameraSetExtTrigSignalType`,
  `CameraSetExtTrigJitterTime`, `CameraSetTriggerDelayTime`,
  `CameraSetTriggerCount`, `CameraSoftTrigger`.

## Camera + bench parameters (from MV-XGC51GC/GM datasheet, edition A1)

- Camera: MV-XGC51GM, **10GigE** (10GBase-T, ~1200 MB/s effective; backward
  compatible 100M/1G/2.5G/5G), IMX426 1/1.7" mono, 816×624 @ 1594.75 fps full
  frame, exposure range **0.8 µs – 838.86 ms**, max analog gain 125×, 1 GB
  in-camera frame buffer, 24 V ±10% supply, <12 W.
- GPIO: 2 opto inputs + 2 opto outputs + 1 non-isolated bidirectional IO.
  Port A pinout: pins 3/4 = `GPI_1±/TRIG_IN±` (default trigger input — the
  Zhongsheng pulse goes here), pins 9/10 = `GPO_1±/STRB_OUT±` (default strobe
  output — drives the LED driver), pins 1/2 power.
- Bench operating point (shipped `mindvisionConfig.json`): pulse generator at
  **5000 Hz determines the frame rate** (camera in ext-trigger mode fires one
  frame per pulse); ROI **512×96** (5000 fps is only achievable at this
  reduced ROI); exposure 1 µs; strobe semi-auto (`strobe_mode: 1`) with
  delay 10 µs, width 35 µs, polarity 1.

## 5000 fps defensive audit (2026-07-31)

Checked and OK as-is:
- **Link bandwidth**: 512×96 mono8 × 5000 fps = 234 MB/s ≪ 1200 MB/s (10GigE).
  Full-frame would be 774 MB/s — still fits. Requires the 10G NIC + CAT6A
  path end-to-end; a 1GbE link caps out at ~2100 fps at this ROI.
- **Timing budget**: frame period 200 µs ≥ strobe delay 10 + width 35 = 45 µs;
  pulse-generator high time at 50 % duty = 100 µs. No overlap into the next
  period.
- **FrameStore**: 5000-slot ring × 49 152 B = 245 MB ≈ exactly 1.0 s of
  buffer at 5000 fps — downstream (processing/recording) must keep up within
  that window.
- **Capture hot path**: `CaptureService::run` now reuses one `Frame` across
  iterations (was per-iteration construction → 48 KB malloc every 200 µs);
  FrameStore slots were already pre-reserved. `checkDeviceHealth` no longer
  grabs frames in triggered modes.
- **Zhongsheng module**: 5000 Hz is inside its 400 Hz–40 kHz range;
  GUI default frequency set to 5000 Hz.
- **Config clamps**: exposure 1.0 µs passes the `> 0` check (sensor min
  0.8 µs); ROI 512×96 is the config default.

Watch on the scope during bring-up (not code issues, but flagged):
- **Strobe vs exposure overlap**: with `exposure_time_us: 1.0` and strobe
  delay 10 µs, the LED pulse window (10–45 µs after trigger) starts after a
  nominally 1 µs exposure would have closed — verify on the scope that the
  sensor's actual exposure window (internal trigger-to-exposure latency)
  overlaps the LED pulse, or tune `acq_trigger_delay_us`/`strobe_delay_us`
  until it does. Illumination-limited imaging usually wants the exposure to
  bracket the flash.
- **Device timestamps**: `frameHead.uiTimeStamp` ticks at 0.1 ms — exactly 2
  ticks per frame at 5000 fps, so device timestamps are coarse (use
  `hostTimestampUs` for latency math, as the pipeline already does). Above
  10 kfps device stamps would duplicate.
- **Gain headroom**: 1 µs exposure is illumination-starved; the sensor allows
  analog gain up to 125× (`analog_gain` clamp already spans it).

## Hardware bring-up checklist (#327)

1. Wiring: module Y1/COM− → camera trigger input (check camera I/O voltage
   spec vs module 3.3/5 V jumper); camera strobe out → illumination/scope;
   USB-RS485 adapter → A/B terminals.
2. Verify generator output on a scope at the set frequency/duty before
   connecting the camera.
3. Free-run regression: default config unchanged behavior.
4. Mode 1: capture idles without frames (health check must NOT kill capture
   after >5 s); each Soft Trigger press → exactly `trigger_count` frames.
5. Mode 2: frame rate tracks generator frequency; Stop (duty 0) → frames
   stop; timed frame count == pulse count; rising vs falling edge; jitter
   filter; `acq_trigger_delay_us` visible on scope (strobe vs trigger edge).
6. Strobe scope check per mode (auto/manual width+delay/polarity).
7. Lifecycle: apply-while-running restarts triggered mode; 10× rapid
   start/stop under external trigger; soft-trigger while stopped → clean
   error dialog.
8. Record results here.

## Follow-up: Linux serial discovery + shared RS485 bus (2026-08-31)

Bench review found the pulse-generator stack unusable on Linux (numeric COM
spinbox, backend-synthesized `COM%1`, empty port enumeration) and
architecturally wrong for multi-drop RS485 (adapter ≠ device). Follow-up spec
lives in the issue #323 comment of 2026-08-31; implementation:

- [[../services/SerialBus]] — `SerialBusManager` + `ModbusBusSession`: one
  exclusive `QSerialPort` owner per (system port name, baud/data/parity/stop),
  serialized transactions, strict correlation (`modbus::classifyResponse`),
  typed `BusError` incl. collision suspicion; `availablePorts()` via
  `QSerialPortInfo` with USB serial/VID/PID identity.
- [[../services/PulseGeneratorService]] — addressed bus client
  (`connect(QString port, SerialSettings, addr)`), typed `LinkError`,
  read-only cancelable `scanBus` classifying generator / generic Modbus
  device / error. Two generators on one adapter = two service instances on
  the shared manager (proved in the pty test).
- [[../services/SyringePumpService]] — transport ported to the shared
  manager; COM-number public API unchanged.
- [[../frontend/ConfigTabs]] — Port dropdown + Refresh, bus-settings combos,
  Addr, Scan (worker thread, cancelable), typed connect errors, QSettings
  persistence with USB-identity re-resolution of renamed device nodes.
- Test `backend.serial_bus_pty` (pty bus simulator) covers the acceptance
  list: system-port-name regression, shared single owner, two generators
  controlled independently, untouched generic device, serialized concurrent
  requests, write-free scan, wrong-address/bad-CRC/short/exception failure
  states, cancel.

Still open (hardware): re-run the bench checklist above on Linux with a real
USB/RS485 adapter; duplicate-address collision behavior is simulated in the
pty test but worth one on-bench confirmation with two modules strapped to the
same address.
