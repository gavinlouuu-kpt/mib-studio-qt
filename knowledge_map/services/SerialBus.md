# SerialBus (shared RS485/Modbus bus layer)

> One `QSerialPort` owner per physical adapter, shared by every Modbus service
> on that bus, with serialized transactions and strict request/response
> correlation. RS485 is multi-drop: an adapter is **not** a device.

**Source:** `src/backend/services/SerialBus.cpp`,
`include/backend/services/SerialBus.h`; pure correlation helpers in
`include/backend/services/ModbusRtu.h` (`expectedFrameLength`,
`classifyResponse`)
**Tests:** `tests/backend/serial_bus_pty_test.cpp` (POSIX pty bus simulator),
`tests/backend/modbus_rtu_test.cpp` (framing primitives)
**Related:** [[PulseGeneratorService]], [[SyringePumpService]],
[[../architecture/AppBackend]], [[../frontend/ConfigTabs]]

## Pieces

- `serialbus::availablePorts()` — cross-platform enumeration via
  `QSerialPortInfo`: system name/location, description, manufacturer, USB
  serial number and VID/PID (identity fields survive `ttyUSB0 → ttyUSB1`
  renames; the GUI persists them for re-resolution). No caching — a Refresh
  action always reflects hot-plug state.
- `SerialSettings` — baud, data bits, parity, stop bits. Together with the
  normalized port name it keys the bus session.
- `SerialBusManager::acquire(portName, settings)` — returns the existing
  session when the key matches, refuses with `PortBusy` when the port is held
  with different settings, opens the port otherwise. Open failures are
  classified from `QSerialPort::error()` (typed enum, never the translated
  `errorString` text): `PermissionError` → `PortBusy`, else
  `PortUnavailable`. Sessions are held via `shared_ptr` with a custom
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

[[PulseGeneratorService]] (Linux-capable `QString` port names) and
[[SyringePumpService]] (still addresses adapters by Windows COM number; the
name is synthesized at its API boundary, transport is the shared session) both
route all serial I/O through here — neither opens a `QSerialPort` directly, so
a pump and a pulse generator on one adapter share the session instead of
fighting over a second open.

## Threading

Each session owns a **dedicated I/O thread**: the `QSerialPort` is created,
used (blocking `waitFor*`, no event loop), and destroyed only on that thread,
so the acquiring thread's Qt event loop can never race the port's internal
buffers. `transact()` marshals the request to the I/O thread and blocks for
the result; any thread may call it, and callers are serialized by the
session's call mutex (always innermost — services take their own state mutex
first).
