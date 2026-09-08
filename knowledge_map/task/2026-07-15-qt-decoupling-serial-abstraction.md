# Qt → React/Tauri migration: backend de-Qt slice 3 (serial abstraction)

Date: 2026-07-15
Epic: #246 · ADR: `docs/decisions/0001-react-tauri-migration.md` ·
Plan: `docs/exec-plans/active/2026-07-15-qt-decoupling-and-tauri-migration.md`

## Context

Third backend de-Qt slice. `SyringePumpService` was the only `QSerialPort` user
in the repo (frontend confirmed clean). Removing it drops `Qt6::SerialPort` from
the backend, leaving the backend on only `Qt6::Core + Qt6::Network`.

## What shipped

- `include/backend/services/ISerialPort.h` — Qt-free serial transport interface
  (byte vectors; fixed 8N1/no-flow-control). `SerialPortFactory` DI seam mirrors
  `CaptureService::CameraFactory`. `makePlatformSerialPort()` +
  `makeSerialPortForPathForTesting()` (POSIX-only test seam).
- `src/backend/services/SerialPortPosix.cpp` (termios: raw 8N1, `select()`
  timeouts, non-blocking reads) and `SerialPortWin32.cpp`
  (`CreateFile`/`SetCommState`/`SetCommTimeouts`), compile-selected via
  `if(WIN32)` in `src/backend/CMakeLists.txt` (stub-swap precedent).
- `SyringePumpService` — `PumpConnection::serial` is now
  `std::unique_ptr<ISerialPort>`; `setSerialPortFactory()` added (defaults to
  the platform port in the constructor). `connect()`, `sendRequest()`,
  `scanModbusAddresses()`, `pollStatus()` all use the interface. Removed
  `<QSerialPort>` / `<QByteArray>` — the `.cpp` and `.h` are fully Qt-free.
- Build: dropped `Qt6::SerialPort` from the `mib_backend` link and from the
  backend-only Qt component set (`MIBDependencies.cmake` → `Core Network`).
- CI: added `dev/react-tauri` to `backend-ci.yml` / `docs-ci.yml` triggers so
  the migration branch and its PRs are gated (they targeted only main/master
  before).

## Tests (headless, no hardware)

- `tests/backend/syringe_pump_fake_serial_test.cpp` — a `FakeSerialPort`
  Modbus-RTU slave (answers FC03 reads with canned registers, echoes FC06, acks
  FC16) injected via the factory. Covers connect → min/max-rate parse →
  setFlowRate/start/stop → pollStatus, plus the no-response (timeout), bad-CRC,
  and wrong-address failure paths. Turns a previously-untested path into
  covered code.
- `tests/backend/serial_port_posix_loopback_test.cpp` — `posix_openpt` pty pair;
  opens the slave via the path seam and round-trips bytes both directions
  through real termios, plus the read-timeout path. POSIX-only (no-op pass on
  Windows).

## Verification

- Full `linux-backend-only` build + `ctest`: **73/73 pass**, including the two
  new serial tests and all integration/e2e mock-camera tests. `ldd` confirms a
  backend test binary links no `Qt6SerialPort`. `check_docs.py` /
  `check_screenshots.py` pass. The Win32 impl is not built on Linux; CI's future
  Windows lane / real hardware covers it.

## Not done here (tracked in the exec-plan)

LUT-catalog `QtNetwork`/paths seam (needs the networking-seam ADR) and the
crash-reporter `QString` glue, then the final `Qt6::Core`/`AUTOMOC` drop —
the Phase 1 exit gate (`linux-backend-only` builds with no Qt SDK).
