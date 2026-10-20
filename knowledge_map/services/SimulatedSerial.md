# SimulatedSerial (serial-sim rig)

> Fake COM ports with Modbus RTU and OEABT devices behind them, startup
> timing faults, and an ownership ledger. Installed process-wide, so the real
> services, transports and discovery probes meet it through their normal code
> paths. It exists to show port races (who opens which port when) before
> they reach the rig.

**Source:** `include/backend/services/SimulatedSerial.h`,
`src/backend/services/SimulatedSerial.cpp` (in the `oeabt_serial` archive, so
every serial user links it); scenario JSON and the environment switch in
`include/backend/services/SimulatedSerialScenario.h`,
`src/backend/services/SimulatedSerialScenario.cpp` (`mib_backend`, needs
nlohmann)
**Tests:** `backend.serial_sim` (`tests/backend/serial_sim_test.cpp`),
`backend.serial_startup_race` (`tests/backend/serial_startup_race_test.cpp`)
**Guide:** `docs/howto/simulated-serial.md`
**Related:** [[ISerialPort]], [[SerialBus]], [[SerialPortClaims]],
[[DeviceDiscoveryService]], [[AutofocusService]],
[[../architecture/AppBackend]], [[../build-and-run/Run-Modes]]

## Responsibility

- `serialsim::install(rig)` makes `makePlatformSerialPort()` and
  `enumerateSerialPorts()` serve the rig (the pump settings dialog lists the
  rig's `comPortNumbers()`; `Tools::availableComPortNumbers()` is portable
  core and stays on the OS); `install(nullptr)` restores the OS. Ports made earlier stay bound to
  their rig.
- `Rig`: `PortSpec` per port (USB identity, `presentAfterMs`,
  `openLatencyMs`, seeded `replyLatencyMs`, `silentAfterOpenMs`,
  `fragmentReplies`, `unpluggedWindowsMs`, `staleHolderUntilMs`), devices per
  port, `unplug`/`replug`, `holdByStaleProcess`/`releaseStaleHolder`.
  Exclusive open by default (Windows semantics on every platform);
  `setExclusiveOpen(false)` allows the second open and records it as
  `SharedOpen`.
- Devices (`SimDevice`): `ModbusRegisterDevice` (FC03/04/06/16, exceptions)
  with presets `makeDlspPump`, `makeTushuiPump`, `makePulseGenerator`
  (plausible identity registers), `makeZc300` (FC04 identity); `OeabtDevice`
  (ASCII commands, framed replies with the V0.5.4 diagnostic line). A frame
  at another baud is never answered (`BaudMismatch`).
- Ledger `Event`s: `Opened`, `OpenDenied` (names the holder thread, how long
  it has held the port and its first frame, or "another process"),
  `SharedOpen`, `OpenFailed`, `Closed`, `ForeignFrame`, `BaudMismatch`,
  `Unplugged`, `Replugged`. `contentionCount()` = denials + shared opens.
- `makeBenchRig(seed)`: the built-in bench (pumps on a CH340 RS485 COM3,
  generator COM4 @ 9600, OEABT on a late CH340 COM5, ZC300 COM6).
- `installFromEnvironment`: `MIB_SERIAL_MODE=sim` (+ `MIB_SERIAL_SIM_SCENARIO`,
  `MIB_SERIAL_SIM_SEED`), called by `AppBackend::initialize` before any
  service exists unless a rig is already installed. A bad scenario or mode
  fails `initialize` and never falls back to real ports.

## Gotchas

- The rig calls devices under its own mutex; device objects need no locks but
  must not call back into the rig.
- `waitForReadyRead` uses a timed wait only: replies are queued solely by the
  same handle's `write()`.
- Under the sim, `AutofocusService::availableEndpoints` offers no CoreMorrow
  endpoints, because that vendor DLL opens COM ports itself.
- A test that installs a rig must `install(nullptr)` before it ends; the
  global outlives `AppBackend`.

## Chip catalog (2026-10-19)

`commonUsbSerialChips()` / `findUsbSerialChip(id)` / `applyChip(spec, chip)`
give a port a real bridge's USB identity, as Windows and Linux report it, and
its timing (FTDI 16 ms latency timer; FTDI and PL2303 split replies).
Covered bridges:
- WCH CH340/CH341/CH343/CH9102;
- FTDI FT232R/FT231X/FT232H/FT2232H/FT4232H;
- Silicon Labs CP2102/CP2105/CP2108;
- Prolific PL2303/PL2303GC;
- Microchip MCP2221;
- CDC-ACM STM32 VCP, Arduino Uno and RP2040;
- an onboard UART (no USB identity).

Scenarios name a chip with `"chip"`, and explicit fields override it.
`backend.serial_startup_race` runs the race matrix over the whole catalog.
`backend.serial_sim` checks the catalog for unique ids and VID:PIDs.
