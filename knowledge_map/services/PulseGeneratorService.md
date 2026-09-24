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
shared [[SerialBus]] session (`SerialBusManager` hands out one
[[ISerialPort]] owner per adapter), so two or more generators — and unrelated Modbus devices —
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
  thread: since #419 the `pulse-generator` provider of
  [[DeviceDiscoveryService]] calls it on a discovery worker (a
  `std::function<bool()>` cancel overload exists for that; the atomic-flag
  overload delegates to it), and [[../frontend/ConfigTabs]] starts a
  discovery job with an explicit `SerialScanScope` instead of owning a thread.
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

## Coordinated illuminated capture (#413)

`beginLiveView`, `enableLiveView`, `endLiveView` own the selected channel for a
capture generation, with identity-scoped tokens and serialized manual access.
Preparation gates output off before frequency changes; enable/off use register
readback. Manual writes/disconnect are refused while owned. Failed off leaves
cached output unchanged and releases manual control for recovery. A recursive
service mutex permits composition of the existing connection/write methods;
serial I/O still belongs to the shared bus worker. `IlluminationSession.h` is
the injected prepare/enable/disable callback contract, not another transport.
See [operator workflow](../../docs/howto/illuminated-live-view.md).


### Automatic default rig setup (September 14 follow-up)

The bundled XGC/R5D profile now enables illuminated Live View with `port: "auto"`,
9600 8N1, address 1, channel 1, 1000 Hz / 2% (20 µs pulse), exposure 100 µs,
rising-edge external trigger and manual strobe 100 µs / zero delay with
polarity 0 (the setting that pulses OUT1 on this rig; see the September 15
measurements). The existing
single-camera discovery selects the camera; Start performs read-only discovery
of USB serial adapters at the configured address on the capture worker. Exactly
one generator-compatible response is required before normal gated startup.
No match or multiple matches produces a specific error; no output is enabled by
discovery. Channel/wiring cannot be discovered electronically: channel 1 is the
known rig preset, not an inferred connection. Custom address/serial/wiring uses
Hardware Setup as an exception. Auto mode re-discovers the adapter each start,
so port renumbering does not require manually saving a new path.

Fresh installs save the bundled profile automatically. Only a byte-structure-
equivalent historical bundled JSON profile at the default path is upgraded;
custom and external profiles are preserved. Explicit saved ports continue to
work unchanged. Discovery exceptions are recorded as camera startup failures
and pass through illumination cleanup. The earlier mandatory one-time manual
setup instructions apply only to custom or ambiguous rigs, not the default rig.
Hardware acceptance of this changed build remains outstanding.


### Automatic adoption keyed on the requested channel (September 14–15, second pass)

`discoverLiveView(config, channel, ports)` adopts a port only when the
identity read has the generator shape, `ScanHit::channelFrequencyRaw[channel]`
is non-zero (`identityChannelConfigured`), and a read-only FC03 of the dLSP
syringe pump's syringe-volume register `0x0061` answers 0 (`readRegisterOnPort`).
Rig facts behind the rule: the module stores 0 Hz for channels never set
(COM6 reads ch1 5000 Hz, ch2–4 0 Hz) and answers 0 for any register outside
its map; a second never-configured module on COM4 answers all zeros; a pump
left channel-enabled has raw 65536 (655.36 Hz) on channel 1 but a non-zero
syringe volume. Ports the bus cannot open because another program holds them
(`LinkError::PortBusy`), modules whose requested channel is unset, and
non-generator responders are each named in the error. Manual `scanBus`
classification is unchanged. Discovery never writes.
