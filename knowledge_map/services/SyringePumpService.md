# SyringePumpService

> Dual-pump control (Sample + Sheath) via Modbus RTU over serial.

**Source:** `src/backend/services/SyringePumpService.cpp`,
`include/backend/services/SyringePumpService.h`
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
  the historical Windows COM-number one, and a `QString` system-port-name one
  (`"ttyUSB0"`, `"COM3"`) that makes Linux adapter sharing reachable; the
  `COMn` synthesis lives only in the int overload. Reconnecting while
  connected releases the old session first (a non-recursive-mutex deadlock
  here is regression-tested in `serial_bus_pty_test`).
- Per-pump control: `setFlowRate`, `setDirection`, `start`, `stop`,
  `purge`, `stopPurge`, `setSyringeVolume`.
- Per-pump status polling: `pollStatus(id)` — UI timer drives this.
- Expose config/status structs (`PumpConfig`, `PumpStatus`).
- Provide `scanModbusAddresses(comPort, baudRate, start, end, timeoutMs)` for
  settings-time address discovery on a selected serial port.

## Enums

- `RunStatus`: Stop (0), Forward (1), Backward (2), Pause (3)
- `Direction`: Infuse (0), Withdraw (1)
- `flowRateUnit` uses integer codes (e.g. `100` = µL/min)

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
`scanModbusAddresses()` obtain ports from the injected `SerialPortFactory`, so a
`FakeSerialPort` can drive the whole Modbus round-trip headless
(`tests/backend/syringe_pump_fake_serial_test.cpp`). This dropped
`Qt6::SerialPort` from the backend link.

Public scan helper probes `REG_RUN_COMMAND` (`0x0001`) with Modbus function
`0x03` over an address range (default 1..8) and returns responsive addresses.

## Threading

Each pump has its own `std::mutex` guarding pump state; per-adapter frame
serialization and response correlation live in the shared [[SerialBus]]
session (bus mutex always innermost). UI calls are synchronous; `pollStatus`
is invoked from the Qt timer in [[../frontend/SyringePumpTab]].

## Gotchas

- `getComPort(id)` is used by [[../frontend/Dialogs]] SyringePumpSettingsDialog
  to avoid double-assigning a COM port to both pumps.
- Dialog-provided `baudRate` and `modbusAddress` must match the hardware.
- See `docs/dLSP_pump.pdf` for pump protocol reference (shipped in repo).
