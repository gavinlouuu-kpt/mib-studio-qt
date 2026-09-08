# ISerialPort

> Qt-free serial transport interface for every Modbus RTU service (syringe
> pumps, pulse generator, the shared [[SerialBus]]). Replaced `QSerialPort`
> so the backend links no Qt at all (epic #246; re-established on the
> shared-backend integration branch after the reliability serial bus landed).

**Source:** `include/backend/services/ISerialPort.h`,
`src/backend/services/SerialPortPosix.cpp` (termios),
`src/backend/services/SerialPortWin32.cpp` (Win32)
**Tests:** `tests/backend/serial_port_posix_loopback_test.cpp` (pty loopback),
`tests/backend/syringe_pump_fake_serial_test.cpp` (fake slave)
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

## Gotchas

- POSIX `waitForReadyRead` uses `select()`; reads are non-blocking (`VMIN=0`,
  `O_NONBLOCK`). `waitForBytesWritten` is `tcdrain()` (no timeout param).
- POSIX `open(comPort)` maps to `/dev/ttyUSB<n>` (best-effort; real deployments
  are Windows). Tests bypass this via the path seam.
- Win32 `waitForReadyRead` polls `ClearCommError().cbInQue`; `\\.\COMn` form is
  used so COM10+ work.
- Adding a new mode (flow control) means extending `SerialSettings` and both
  platform ports, not reaching for Qt.
