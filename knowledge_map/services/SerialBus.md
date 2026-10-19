# SerialBus (shared RS485/Modbus bus layer)

> One serial-port owner (an [[ISerialPort]], Qt-free) per physical adapter,
> shared by every Modbus service on that bus, with serialized transactions and
> strict request/response correlation. RS485 is multi-drop: an adapter is
> **not** a device.

**Source:** `src/backend/services/SerialBus.cpp` (compiled into the
`oeabt_serial` archive with the platform [[ISerialPort]], so drivers and CLIs
link it without `mib_backend`), `include/backend/services/SerialBus.h`; pure correlation helpers in
`include/backend/services/ModbusRtu.h` (`expectedFrameLength`,
`classifyResponse`)

**Function codes:** FC03 read holding, FC04 read input (`buildReadInputRequest`,
added for the ZC300 Z stage, #464 — identity/status registers that reject
FC03), FC06 write single, FC16 write multiple. FC03 and FC04 share one frame
layout, length rule and correlation path; any other function code is
unframeable (`expectedFrameLength` → -2) and fails the transaction.
**Tests:** `tests/backend/serial_bus_pty_test.cpp` (POSIX pty bus simulator),
`tests/backend/serial_bus_deadline_test.cpp` (`backend.serial_bus_deadline`,
#720: held port call, fail-fast, late result and late reply never answer a
later request, concurrent stress), `tests/backend/modbus_rtu_test.cpp`
(framing primitives)
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
  via `modbus::expectedFrameLength`. It never waits without limit
  (Deadlines, below). `Transaction::detail` says why a transaction
  failed ("no response within 1000 ms", the port's "serial output timed out:
  ..." / "serial write timed out ...", "no result from the serial I/O thread
  within 3510 ms ..."); services put it in their messages.

## Deadlines (#720)

Every step of a transaction is bounded by the port ([[ISerialPort]]): the
write by `kSerialWriteTimeoutMs` (1 s), the drain by `timeoutMs` (a drain
or write timeout is `WriteFailed`, and the port has discarded the unsent
bytes), the response by `timeoutMs`. On top of that, `transact()` waits for
the I/O thread at most `transactBudgetMs(timeoutMs)` = inter-frame delay +
write bound + 2 x `timeoutMs` + collision listen + 500 ms slack (3510 ms for
the pumps' 1000 ms). A transaction that runs to its own deadlines is never
cut short; only a port call that ignores its bound (a driver stuck in the
kernel, a fake that blocks) trips it. Then:

- the caller gets `Timeout` ("no result from the serial I/O thread ...") and
  its locks are released (a pump command returns);
- every request carries a **sequence number**: the I/O thread's late result
  for an abandoned request is dropped (logged as a warning, with its
  outcome), never handed to the next caller; a request the I/O thread had
  not taken yet is withdrawn and never sent;
- while the abandoned request still holds the I/O thread, further calls fail
  fast with `Timeout` ("the serial I/O thread is still stuck in an earlier
  transaction"; nothing is queued or sent);
- **recovery:** once the stuck call returns, the session works again with no
  reopen. Any late reply bytes on the wire are drained by the next
  transaction before it writes (the stale-drain step above), so they are
  never attributed to it. A reply that arrives after that drain is
  indistinguishable from an answer: Modbus RTU frames carry no transaction id.
- The session destructor still joins the I/O thread, so closing a session
  waits for a stuck port call to return (bounded for the platform ports).

The handoff is all under `jobMutex_` (condition variable `wait_until`, no
`std::timed_mutex`), clean on the TSan lane.

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
overload takes a system port name) both route all serial I/O through here, as
does the [[ZC300Stage]] driver (ADR 0013, #464) —
neither opens a port directly, so
a pump and a pulse generator on one adapter share the session instead of
fighting over a second open.

## Threading

Each session owns a **dedicated I/O thread**: the `ISerialPort` is created
(from the manager's factory), opened with `openNamed(portName, settings)`,
used (bounded `waitFor*`), and destroyed only on that thread. `transact()` marshals a copy of the request to the I/O thread and
waits up to `transactBudgetMs(timeoutMs)` for the result; any thread may call
it, and callers are serialized by the session's call mutex (always innermost —
services take their own state mutex first).
