# SyringePumpService

> Dual-pump control (Sample + Sheath) via Modbus RTU over serial. Each slot
> holds either a Longer dLSP501 syringe pump or a Tushui peristaltic pump.

**Source:** `src/backend/services/SyringePumpService.cpp`,
`include/backend/services/SyringePumpService.h`,
`include/backend/services/TushuiPumpProtocol.h` (peristaltic register map)
**Related:** [[SerialBus]] (transport), [[../frontend/SyringePumpTab]],
[[../frontend/Dialogs]] (SyringePumpSettingsDialog)

## Responsibility

- Maintain two independent [[ISerialPort]] connections
  (`PumpId::Sample`, `PumpId::Sheath`), created via an injected
  `SerialPortFactory` (defaults to the platform port).
- Maintain two independent pump connections (`PumpId::Sample`,
  `PumpId::Sheath`). Serial I/O goes through the shared [[SerialBus]]
  session for each adapter (acquired from the `AppBackend`-owned
  `SerialBusManager`), so a pump can share an RS485 adapter with other Modbus
  devices instead of failing on a second open. `connect()` has two overloads:
  the historical Windows COM-number one, and a `std::string` system-port-name one
  (`"ttyUSB0"`, `"COM3"`) that makes Linux adapter sharing reachable; the
  `COMn` synthesis lives only in the int overload. Reconnecting while
  connected releases the old session first (a non-recursive-mutex deadlock
  here is regression-tested in `serial_bus_pty_test`).
- Per-pump control: `setFlowRate`, `setDirection`, `start`, `stop`,
  `purge`, `stopPurge`, `setSyringeVolume`.
- Per-pump status polling: `pollStatus(id)` — UI timer drives this.
- Run facts for the run snapshot (#712): `runFacts(id, wait, out)` returns the
  slot's connection, configuration and last poll readback (`PumpReadback`:
  `valid`, `lastPollOk`, steady-clock `at`, `runStatus`, optional
  `speedRpm`, `realtimeFlowRaw`, `accumulatedVolumeRaw`, `stalled`). It waits at
  most `wait` for the pump lock and returns false when the pump is busy with
  serial I/O; it never talks to the pump. The readback is only replaced by a
  poll in which every register answered (a dLSP poll whose direction read
  failed and was assumed counts as incomplete); every poll sets `lastPollOk`.
  Connect and disconnect clear it. Peristaltic readbacks carry `runStatus` and
  `speedRpm` only (its `accumulatedVolume` is integrated by the host, not
  reported); dLSP readbacks carry the raw flow/volume registers, whose units
  the protocol PDF does not state. `RunFacts::applied` (`AppliedSettings`:
  optional flow rate, unit, direction) holds only what the pump is known to
  hold: set by a successful write (`setFlowRate`, the dLSP value as written;
  `setDirection`; a peristaltic `start` writes the direction) or a read-back
  (the peristaltic connect reads speed and direction), cleared on connect,
  disconnect and before each write (a failed write leaves it unknown); a
  peristaltic purge clears it and the restore at stop sets it again. A dLSP
  connect reads neither, so a pump connected but not configured has none:
  `PumpConfig` is the operator's request (and may be left from another
  device), not evidence. The coordinator records these in snapshot v3
  ([[../data-model/HDF5-Storage]]).
- Expose config/status structs (`PumpConfig`, `PumpStatus`).
- Provide `scanModbusAddresses(comPort, baudRate, start, end, timeoutMs)` for
  settings-time address discovery on a selected serial port.

## Enums

- `RunStatus`: Stop (0), Forward (1), Backward (2), Pause (3)
- `Direction`: Infuse (0), Withdraw (1)
- `PumpModel`: DlspSyringe (0), TushuiPeristaltic (1) — contract `pump_models`
- `flowRateUnit` uses integer codes (e.g. `100` = µL/min)

## Pump models (2026-10-04)

`connect(id, portName, baud, address, model, microlitersPerRev)` picks the
device for a slot; the other overloads connect a dLSP. The control surface is
the same for both; the peristaltic paths (`peristaltic*` helpers) map it onto
the Tushui simplified register set 100-107 (protocol V2.21, see
`docs/integration/tushui-peristaltic-pump.md`):

| Operation | Peristaltic behaviour |
|---|---|
| connect | Reads 100-107 only (no write); adopts speed/direction; flow limits = 0.01-500 rpm x µL/rev |
| setFlowRate | µL/min or mL/min -> rpm = flow / µL/rev -> reg 100 (rpm x100); rates outside 0.01-500 rpm **fail** (no clamp) |
| setDirection | Infuse = counter-clockwise (reg 101 = 1), Withdraw = clockwise; confirmed on the PZ7035 bench 2026-10-05 for both pumps (slave 3 Sample, slave 4 Sheath) |
| start | turns (102-103) = 0 so the run is continuous, then reg 104 = 1 |
| purge | 100 rpm in the purge direction; `stop`/`stopPurge` restore the flow speed and direction |
| pollStatus | Run state + direction -> `RunStatus`; live flow = rpm x µL/rev; `accumulatedVolume` integrated in µL between polls (the pump has no counter) |
| setSyringeVolume | fails (not applicable) |
| disconnect | always writes stop |

Default calibration 25 µL/rev (0.4 rpm = 10 µL/min, operator figure until a
measured calibration). Test: `backend.peristaltic_pump_fake_serial`.

## Modbus helpers

Private: CRC-16, `buildReadRequest`, `buildWriteSingleRequest`,
`buildWriteMultipleRequest`, big-endian ABCD float ↔ two 16-bit registers,
`readHoldingRegisters`, `writeSingleRegister`,
`writeMultipleRegisters`.

The pure framing primitives live in `include/backend/services/ModbusRtu.h`
(`backend::services::modbus`), unit-tested by
`tests/backend/modbus_rtu_test.cpp`. As of the Qt-decoupling work (epic #246)
this service is **fully Qt-free**: frames are `std::vector<uint8_t>`
(`modbus::Frame`, not `QByteArray`) and transport goes through [[ISerialPort]]
(POSIX termios / Win32) instead of `QSerialPort`. `connect()` and
`scanModbusAddresses()` acquire sessions from the shared [[SerialBus]]
manager, whose injected `SerialPortFactory` lets a `FakeSerialPort` drive the
whole Modbus round-trip headless
(`tests/backend/syringe_pump_fake_serial_test.cpp`). This keeps
`Qt6::SerialPort` (and Qt Core) out of the backend link.

Public scan helper probes `REG_RUN_COMMAND` (`0x0001`) with Modbus function
`0x03` over an address range (default 1..8) and returns responsive addresses.

## Threading

Each pump has its own `std::timed_mutex` guarding pump state (timed since
#712 so `runFacts` can give up instead of waiting out a 1 s serial timeout
that `pollStatus` holds the lock across); per-adapter frame
serialization and response correlation live in the shared [[SerialBus]]
session (bus mutex always innermost). UI calls are synchronous; `pollStatus`
is invoked from the Qt timer in [[../frontend/SyringePumpTab]].

## Gotchas

- `getComPort(id)` is used by [[../frontend/Dialogs]] SyringePumpSettingsDialog
  to avoid double-assigning a COM port to both pumps.
- Dialog-provided `baudRate` and `modbusAddress` must match the hardware.
- dLSP `setFlowRate` writes `clamp(rate, 1, 9999)` truncated to an integer
  but keeps the unclamped rate in `PumpConfig`; the snapshot's setpoint is
  the written value (`AppliedSettings`).
- See `docs/dLSP_pump.pdf` for pump protocol reference (shipped in repo).
- Peristaltic: which way Infuse pushes liquid depends on how the tubing is
  loaded in the head; swap the tubing ends rather than the mapping.
- The PZ7035 instrument pump answers at slave address 3 on `/dev/ttyPS1`
  (vendor default is 1).
