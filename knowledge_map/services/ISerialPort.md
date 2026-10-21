# ISerialPort

> Qt-free serial transport interface for every Modbus RTU service (syringe
> pumps, pulse generator, the shared [[SerialBus]]). Replaced `QSerialPort`
> so the backend links no Qt at all (epic #246; re-established on the
> shared-backend integration branch after the reliability serial bus landed).

**Source:** `include/backend/services/ISerialPort.h`,
`src/backend/services/SerialPortPosix.cpp` (termios),
`src/backend/services/SerialPortWin32.cpp` (Win32)
**Tests:** `tests/backend/serial_port_posix_loopback_test.cpp` (pty loopback),
`tests/backend/serial_port_posix_timeout_test.cpp` (`backend.serial_port_posix_timeout`,
#720: the real POSIX port with `ioctl`/`tcflush` faked and `write(2)` behind
the `MIB_SERIAL_SYS_WRITE` seam, on a pty — output that never drains, endless
`EAGAIN` via `tcflow(TCOOFF)`, endless `EINTR` from the queue query and from
`write`, `interrupt()` from another thread, recovery),
`tests/backend/syringe_pump_fake_serial_test.cpp` (fake slave),
`tests/backend/serial_port_enumeration_test.cpp` (`backend.serial_port_enumeration`:
well-formed unique names, `serialbus::availablePorts()` mirror; with
`MIB_TEST_EXPECT_PORT=COM6` on the bench it checked the nanopositioner's USB
VID/PID/serial against Windows PnP)
**Related:** [[SyringePumpService]], [[../build-and-run/Dependencies]]

## Responsibility

- Abstract byte-level serial I/O behind a pure-virtual interface so no Qt type
  crosses the backend boundary. Payloads are `std::vector<uint8_t>` (the Modbus
  framing in `ModbusRtu.h` is already vector-based).
- `SerialSettings {baudRate, dataBits, parity, stopBits}` lives here;
  `open(comPort, baud)` is the 8N1 convenience, `openNamed(systemName,
  settings)` the full form ("COM3", "\\.\COM12", "ttyUSB0", "/dev/pts/4").
  The default `openNamed` maps a trailing COM number onto `open()`, so fakes
  that only implement `open()` keep working under the bus layer.
- `enumerateSerialPorts()` — Qt-free port listing (`SerialPortInfo`: name,
  location, description, manufacturer, USB serial/VID/PID) via SetupAPI on
  Windows (`setupapi` linked into `mib_backend`) and sysfs on Linux.

## Key APIs

No serial call blocks without limit (#720). On both platforms:

- `write(data)` gives up after `kSerialWriteTimeoutMs` (1000 ms, in
  `ISerialPort.h`) when the driver takes no more bytes. Win32 uses it as the
  COM write timeout (`WriteTotalTimeoutConstant`); a short `WriteFile` is
  reported as a timeout. POSIX writes non-blocking and, on `EAGAIN`, waits for
  room with `poll(POLLOUT)` up to the remaining time (the old loop retried
  `EAGAIN` for ever). Either way the error names the timeout ("serial write
  timed out after 1000 ms (k of n bytes taken, discarded)").
- `waitForBytesWritten(ms)` polls the output queue to a steady-clock deadline
  (1 ms steps): Windows `COMSTAT.cbOutQue` via `ClearCommError` (never
  `FlushFileBuffers`, which ignores COM write timeouts), POSIX `ioctl(TIOCOUTQ)`
  (never `tcdrain()`, which has no timeout). A failing query returns the
  driver's error at once; a platform without `TIOCOUTQ` would wait the frame's
  nominal line time (capped at `ms`) and report the drain as unconfirmed.
- POSIX checks the deadline on **every** retry, `EINTR` included (a signal
  storm, or a driver that keeps answering `EINTR`, still times out).
- `interrupt()` (#720 review): the one call allowed from another thread while
  the owner is inside a call (never concurrently with `open*`/`close`). It
  makes the call in progress and every later wait, write or drain fail at
  once ("serial port interrupted (session closing)") and discards the unsent
  output; `close()` after it flushes output first so a tty close does not
  wait out `closing_wait`. POSIX wakes a `poll`/`select` wait through a
  non-blocking self-pipe created with the port; Win32 calls `CancelIoEx` on
  the handle and the polling waits check the flag. The fd/handle stays owned
  and is closed by the owner thread. Default no-op, so fakes need nothing.
  [[SerialBus]] uses it to tear a session down without waiting out a stalled
  call.
- On either timeout the untransmitted output is **discarded** (POSIX
  `tcflush(TCOFLUSH)`, Win32 `PurgeComm(PURGE_TXABORT | PURGE_TXCLEAR)`), best
  effort, so a command the caller already reported as failed cannot reach the
  device later when a stalled adapter recovers. `lastSystemError()` is
  `ETIMEDOUT` / `ERROR_TIMEOUT`.

`backend.serial_port_win32_timeout` tests a stalled driver (and the purge),
successful drain, unplug error, and port release using the real
implementation with fake Win32 calls; `backend.serial_port_posix_timeout` does
the same for POSIX.

- `open(int comPort, int baudRate)` / `close()` / `isOpen()`
- `write(const std::vector<uint8_t>&) -> int` (bytes written, -1 on error)
- `waitForBytesWritten(int ms)`, `waitForReadyRead(int ms)`, `readAll()`
- `lastError()`, `lastSystemError()` (OS code of the last failed open; the
  bus layer classifies `PortBusy` from it)
- `SerialPortFactory = std::function<std::unique_ptr<ISerialPort>()>` — the DI
  seam (mirrors `CaptureService`'s `CameraFactory`). `SerialBusManager`
  defaults it to `makePlatformSerialPort()`; tests inject a fake through
  `SerialBusManager::setSerialPortFactory()` (`AppBackend::serialBus()`).
- `makePlatformSerialPort()` — POSIX (termios) or Win32, selected in
  `src/backend/CMakeLists.txt` (`if(WIN32)` source swap).
- `makeSerialPortForPathForTesting(path, baud)` — POSIX-only test seam that
  opens an explicit device path (e.g. a pty slave); returns nullptr on Windows.

## Simulated serial hardware (2026-10-19)

While a [[SimulatedSerial]] rig is installed (`serialsim::install`, or
`MIB_SERIAL_MODE=sim` through `AppBackend::initialize`), both platform files
return the rig's port from `makePlatformSerialPort()` and its ports from
`enumerateSerialPorts()`, so every caller is simulated without a factory
seam of its own. Nothing changes when no rig is installed.

## Gotchas

- POSIX `waitForReadyRead` uses `select()`; reads and writes are non-blocking
  (`VMIN=0`, `O_NONBLOCK`). `waitForBytesWritten` polls `TIOCOUTQ`, which
  counts the kernel/driver queue only: bytes already in a USB adapter's own
  FIFO are not seen (a drain may be confirmed a byte time early), and the
  discard after a timeout cannot recall them.
- A pty never models a stalled drain: its `TIOCOUTQ` (and `tcdrain`) report
  empty at once. Stop its output with `tcflow(TCOOFF)` to model a full queue
  (every write `EAGAIN`).
- POSIX `open(comPort)` maps to `/dev/ttyUSB<n>` (best-effort; real deployments
  are Windows). Tests bypass this via the path seam.
- Win32 `waitForReadyRead` polls `ClearCommError().cbInQue`; `\\.\COMn` form is
  used so COM10+ work.
- Adding a new mode (flow control) means extending `SerialSettings` and both
  platform ports, not reaching for Qt.
