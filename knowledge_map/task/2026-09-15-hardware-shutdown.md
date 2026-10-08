# Hardware ownership and desktop shutdown

## Incident evidence

The user reported that the nanopositioner endpoint could not open. Windows
reported two installed desktop processes, one with no visible main window.
Earlier logs show successful OEABT connections on COM7. A read-only stack
inspection of the hidden process found its main thread in
`QCoreApplication::exec` / the Windows event dispatcher, not a shutdown join.
This does **not** establish that a previous close deadlocked, nor which process
owned COM7. Why that particular instance was hidden remains unconfirmed.

Local diagnostic evidence: `data/logs/shutdown-hidden-stacks.txt`.

## Reproduced defects and changes

- Closing MainWindow with another top-level widget visible left Qt's event
  loop running. Accepted close now drains discovery, shuts down the backend,
  and queues application quit. `aboutToQuit` also drains discovery and shuts
  down hardware for other exit paths.
- Explicit `AppBackend::shutdown()` stopped the pipeline but retained pump
  and generator connections until destruction. It now explicitly disconnects
  the nanopositioner, both pumps, and generator after capture/processing stop.
  Existing safe-voltage, pump-stop, and capture-owned LED-off behavior remains
  in the owning services. Logs identify each shutdown stage.
- Windows `waitForBytesWritten(timeout)` called `FlushFileBuffers`, which
  ignores serial write timeouts. It now polls `COMSTAT.cbOutQue` to a steady
  clock deadline and propagates timeout/device errors. This applies to OEABT
  and the shared Modbus bus (pumps and generator).
- [[../frontend/DesktopInstance]] prevents duplicate cooperating desktops
  from acquiring hardware. Live locks do not expire with age; dead owners
  recover automatically.
- `DeviceInitManager::stop()` cancels timers and queued results, skips further
  nanopositioner probes after cancellation, and drains active futures before
  hardware teardown. A stopped manager cannot restart discovery.

Microsoft's serial I/O documentation explains the unbounded flush behavior:
https://learn.microsoft.com/en-us/windows/win32/devio/read-and-write-operations

## Verification

Regression-first evidence: the fake Windows driver hit the five-second test
watchdog while waiting for a 20 ms write timeout; the shared-port shutdown and
main-window close tests failed before their fixes. Logs:
`data/logs/shutdown-regression-before.log`.

New guards: `backend.serial_port_win32_timeout`, `backend.hardware_shutdown`
(ten shared-port connect/shutdown cycles), `frontend.mainwindow_shutdown`,
`frontend.desktop_instance`. All four pass in the Windows build.

Full Windows build and 116 non-hardware/non-performance tests pass. Suite
evidence: `data/logs/shutdown-suite.log`. Live hardware and the installed
Program Files executable have not been modified by this task.

## Limits

No claim is made that arbitrary vendor DLL calls can be cancelled: an active
discovery call must return before its future drains. The POSIX transport's
`tcdrain` remains separate work. This change prevents cooperating duplicate
launches, not external vendor tools taking a port. Tests use simulated serial
devices; live hardware was not interrupted. ThreadSanitizer requires the Linux
CI lane; the local Windows/MSVC environment cannot run it.

## Build and test targets (moved from Build.md, 2026-09-21)

Hardware shutdown regression targets (2026-09-15):
`serial_port_win32_timeout_test` (Windows-only native transport fault injection),
`hardware_shutdown_test` in `mib_backend_tests`, `mainwindow_shutdown_test`,
and `desktop_instance_test`. The two desktop lifecycle tests are standalone
because they run Qt event loops / subprocesses. Their AUTOUIC is disabled to
keep `mib_frontend_common` the sole owner of generated UI headers.
