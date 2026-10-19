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
later request, concurrent stress, teardown with the I/O thread held: joined
after `interrupt()`, detached after `kShutdownJoinMs` when the call ignores
it; the port refused while the detached thread holds it and reopened after,
another port opening during the teardown, an acquire racing a normal close,
the late warning surviving `spdlog::drop_all()`), `tests/backend/modbus_rtu_test.cpp`
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
  `EBUSY` / `EACCES` / `EPERM` (POSIX) → `PortBusy`, else `PortUnavailable`. Sessions are held via `shared_ptr`
  (one service disconnecting never yanks the port from another). The
  registry keeps a weak reference to the session and to its **port
  reservation**, which the session's I/O thread holds until it has closed
  the port, so a dying session and a fresh acquire never hold the device at
  once. A port whose last session is still closing: `acquire()` waits for
  the destructor's outcome (at most `kShutdownJoinMs` + 250 ms, never under
  the registry lock), then opens it; once the destructor has detached a
  stalled thread, `acquire()` refuses at once with `PortBusy`, detail
  "serial port <name> is still closing after a stalled transaction", until
  the thread releases the reservation right after its close. The deleter is
  a plain `delete`: the registry lock is never held during a teardown, so
  other ports open meanwhile (#720).
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
- **Teardown is bounded.** Dropping the last reference (a pump disconnect,
  or a connect that failed on a stalled first transaction, both under the
  pump and bridge locks) never waits for a stuck call: the destructor sets
  stop, calls `ISerialPort::interrupt()` when a transaction is still running
  (the platform ports wake a `poll`/`select` wait at once through a
  self-pipe or `CancelIoEx`, fail it and discard the unsent bytes), and
  waits at most `kShutdownJoinMs` (1 s) for the I/O thread. The thread
  closes the port itself, on its own thread: the fd is never closed from
  another thread, where its number could be reused and receive a late pump
  frame. A thread still inside a port call after the bound (stuck in the
  driver, or a fake that ignores `interrupt()`) is **detached** and logged.
  It shares ownership (`shared_ptr<IoState>`) of everything it touches (the
  port, the request copy, the sequence and result state), so it never uses
  freed memory; once the call returns it sees stop, closes the port and
  leaves. Until then the port stays reserved: a new session on it is
  refused, not opened beside the old one (registry, above).
- **Logging outlives the logging setup.** The session and its I/O thread log
  through the logger captured when the session was created (the default
  logger, or a null sink when there is none), with `SPDLOG_LOGGER_*`, never
  through the global macros: a detached thread can still log after the
  application dropped its loggers (`spdlog::drop_all()` / `shutdown()` at
  exit), when the global default is null. The ports, the factory and the
  Modbus helpers do not log on that path; the manager's own lines run on the
  caller's thread and use the global macros.

All handoff state lives in `ModbusBusSession::IoState` under its mutex
(condition variable `wait_until` / `wait_for`, no `std::timed_mutex`), clean
on the TSan lane and under ASan for the detached path.

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
used (bounded `waitFor*`), and closed and destroyed only on that thread
(the destructor may only `interrupt()` it). `transact()` marshals a copy of the request to the I/O thread and
waits up to `transactBudgetMs(timeoutMs)` for the result; any thread may call
it, and callers are serialized by the session's call mutex (always innermost —
services take their own state mutex first).
