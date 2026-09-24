# SerialBus (shared RS485/Modbus bus layer)

> One serial-port owner (an [[ISerialPort]], Qt-free) per physical adapter,
> shared by every Modbus service on that bus, with serialized transactions and
> strict request/response correlation. RS485 is multi-drop: an adapter is
> **not** a device.

**Source:** `src/backend/services/SerialBus.cpp`,
`include/backend/services/SerialBus.h`; pure correlation helpers in
`include/backend/services/ModbusRtu.h` (`expectedFrameLength`,
`classifyResponse`)
**Tests:** `tests/backend/serial_bus_pty_test.cpp` (POSIX pty bus simulator),
`tests/backend/modbus_rtu_test.cpp` (framing primitives)
**Related:** [[PulseGeneratorService]], [[SyringePumpService]],
[[../architecture/AppBackend]], [[../frontend/ConfigTabs]]

## Pieces

- `serialbus::availablePorts()` — Qt-free enumeration
  (`ISerialPort.h` `enumerateSerialPorts()`: Win32 SetupAPI COM-port class,
  Linux sysfs): system name/location, description, manufacturer, USB serial
  number and VID/PID (identity fields survive `ttyUSB0 → ttyUSB1` renames;
  the GUI persists them for re-resolution). No caching — a Refresh action
  always reflects hot-plug state. Strings are `std::string`; the Qt
  [[../frontend/ConfigTabs]] converts at its boundary.
- `SerialSettings` (defined in `ISerialPort.h`, aliased here) — baud, data
  bits, parity, stop bits. Together with the normalized port name it keys the
  bus session.
- `SerialBusManager::setSerialPortFactory(factory)` — the DI seam: sessions
  opened afterwards construct their port from it (default
  `makePlatformSerialPort()`); tests inject a fake Modbus slave
  (`syringe_pump_fake_serial_test`, `pump_bridge_facade_test`) through
  `AppBackend::serialBus()`.
- `SerialBusManager::acquire(portName, settings)` — returns the existing
  session when the key matches, refuses with `PortBusy` when the port is held
  with different settings, opens the port otherwise. Open failures are
  classified from `ISerialPort::lastSystemError()` (the OS code, never the
  text): `ERROR_ACCESS_DENIED` / `ERROR_SHARING_VIOLATION` (Win32) and
  `EBUSY` / `EACCES` / `EPERM` (POSIX) → `PortBusy`, else `PortUnavailable`. Sessions are held via `shared_ptr` with a custom
  deleter that closes the port and erases the registry entry atomically
  under the registry lock — one service disconnecting never yanks the port
  from another, and a dying session cannot interleave with a fresh acquire
  of the same port.
- `ModbusBusSession::transact(request, timeoutMs)` — the only I/O path.
  Callers are serialized; drains stale bytes first, enforces the RTU
  inter-frame delay **measured from the last bus activity** (an idle bus
  pays nothing), then reads exactly one frame, framed from its own header
  via `modbus::expectedFrameLength`.

## Strict correlation (`modbus::classifyResponse`)

A response is accepted only when CRC, slave address, function code, and the
frame's own length/byte-count/echo fields all match the outstanding request.
Everything else maps to a typed `BusError`:

| Wire behavior | Result |
|---|---|
| silence | `Timeout` |
| complete frame from another address | discarded, keep reading (final: `WrongAddress`) |
| CRC mismatch | `CrcError` (possible duplicate-address collision) |
| unframeable / truncated bytes | `FrameError` / `Timeout` |
| right device, wrong function | `WrongFunction` |
| Modbus exception frame | `ModbusException` + code |
| trailing bytes after a valid frame | `CollisionSuspected` |

Stale/delayed frames (a device answering after a previous transaction's
deadline) are drained or discarded — never attributed to the addressed device.

## Users

[[PulseGeneratorService]] (system port names as `std::string`) and
[[SyringePumpService]] (COM-number overload synthesizes `COMn`; string
overload takes a system port name) both route all serial I/O through here —
neither opens a port directly, so
a pump and a pulse generator on one adapter share the session instead of
fighting over a second open.

## Threading

Each session owns a **dedicated I/O thread**: the `ISerialPort` is created
(from the manager's factory), opened with `openNamed(portName, settings)`,
used (blocking `waitFor*`), and destroyed only on that thread. `transact()` marshals the request to the I/O thread and blocks for
the result; any thread may call it, and callers are serialized by the
session's call mutex (always innermost — services take their own state mutex
first).
