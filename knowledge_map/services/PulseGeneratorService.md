# PulseGeneratorService

> Controls a Zhongsheng (中盛科技) pulse frequency & duty-cycle output module
> over RS485 Modbus RTU — the TTL pulse source for the MindVision camera's
> **external acquisition trigger** (NOT the sort-output pulse; that is
> [[TriggerService]]).

**Source:** `src/backend/services/PulseGeneratorService.cpp`,
`include/backend/services/PulseGeneratorService.h`
**Tests:** `tests/backend/pulse_generator_frame_test.cpp` (known-answer frames
from the vendor manual), `tests/backend/serial_bus_pty_test.cpp`
(pty-simulated bus: discovery, multi-device control, failure classification)
**Related:** [[SerialBus]] (transport), [[SyringePumpService]],
[[../camera/MindVisionCamera]], [[../frontend/ConfigTabs]],
[[../architecture/AppBackend]]

## Device identity

`(physical bus, serial settings, Modbus slave address)` — the pulse-generator
**channel** is a setting below that identity. The service is a *client* of a
shared [[SerialBus]] session (`SerialBusManager` hands out one `QSerialPort`
owner per adapter), so two or more generators — and unrelated Modbus devices —
can share one USB/RS485 adapter at different addresses. A second generator on
the same bus is a second `PulseGeneratorService` instance built on the same
`SerialBusManager`.

## Responsibility

- `connect(portName, SerialSettings, addr)` — takes a **system port name**
  (`"ttyUSB0"`, `"/dev/ttyACM0"`, `"COM3"`); never synthesizes `COMn`, so it
  works on Linux (regression-tested over a pty). Acquires the shared bus
  session, then verifies the addressed device by reading all channel registers
  (FC03) and seeds state from hardware **without writing** — an
  already-pulsing generator keeps pulsing across a control-link reconnect.
- `scanBus(portName, settings, from, to, cancel, timeout, error*)` —
  read-only FC03 discovery over a bounded address range (GUI default 1–16),
  cancelable between addresses; **never emits a write function code**.
  Classifies each responding address as `PulseGenerator` (expected
  register-map shape **and plausible values** — see below), `ModbusDevice`
  (valid response, different shape/exception/implausible values), or `Error`
  (corrupt/possible duplicate-address collision). A port that cannot be
  acquired at all is reported through the `error` out-param so the GUI can
  distinguish it from a silent bus. Synchronous — callers run it off the GUI
  thread ([[../frontend/ConfigTabs]] uses a worker `std::thread`).
- `identityLooksLikeGenerator(data)` — plausibility gate on the 12-register
  identity read: per channel, frequency raw must be 0 or within
  [400 Hz, 40 kHz]×100 and duty raw ≤ 10000. Both `connect()` (refuses with
  `IncompatibleDevice`) and `scanBus()` apply it, so an unrelated Modbus
  device that merely serves 12 holding registers at address 0 is never
  adopted and later written into.
- `setFrequency(ch, hz)` — clamps to the module's 400 Hz–40 kHz range, writes
  u32 = Hz×100 (high word first) via FC16.
- `setDutyCycle(ch, %)` — clamps 0–100, u16 = %×100 via FC06. While the
  channel is gated off, the value is cached and written on the next enable.
- `setOutputEnabled(ch, on)` — the module has **no run/stop register**: off
  writes duty 0 % (line idles low), on restores the configured duty.
- `disconnect()` releases the shared session (the adapter only closes when its
  last client lets go) and deliberately leaves outputs untouched.
- `lastError()` / `Status.lastError` — typed `LinkError` so the GUI can
  distinguish port-unavailable / port-busy / timeout / CRC-frame error /
  Modbus exception / address collision / incompatible device.

## Device protocol (vendor manual 脉冲频率与占空比输出系列 V2.0)

- Modbus RTU, factory default address 1, 9600 8N1; FC 03/04/06/10.
- Channel N (0-based): frequency high word at holding register `3N`, low word
  at `3N+1` (u32 = Hz×100), duty at `3N+2` (u16 = %×100).
- Range 400 Hz–40 kHz, duty 0–100 %, resolution 0.01 Hz / 0.01 %; optocoupler
  outputs, 3.3/5 V high level via internal jumper (Y1–Y4 vs COM−).
- Device params (persisted, power-cycle to apply): 0x32 station address,
  0x33 baud code, 0x3D parity — not exposed by the service.
- Known-answer vectors (manual §2.3, pinned in the test): ch1 1000 Hz →
  `01 10 00 00 00 02 04 00 01 86 A0 C0 77`; ch1 50 % → `01 06 00 02 13 88 25 5C`.

## Encoding helpers

`clampFrequency` / `clampDuty` / `frequencyToRegisterValue` /
`dutyToRegisterValue` / `buildFrequencyFrame` / `buildDutyFrame` are pure
statics (unit-testable without a serial port); framing and strict response
correlation reuse `backend::services::modbus::*` from `ModbusRtu.h`.

## Wiring

Owned by [[../architecture/AppBackend]] (`pulseGenerator()` accessor,
constructed against the backend-owned `SerialBusManager`), driven from the
MindVision section of [[../frontend/ConfigTabs]] (port dropdown + refresh,
bus settings, address, scan, connect, frequency/duty, start/stop). Compiles on
every platform — it has no MindVision SDK dependency.
