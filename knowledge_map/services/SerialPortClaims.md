# SerialPortClaims (in-process port arbitration)

> Keeps discovery probes off serial ports that a service holds, or that
> config.json assigns to the pumps. Makes a service wait for a probe that is
> already on its port instead of failing "busy".

**Source:** `include/backend/services/SerialPortClaims.h`,
`src/backend/services/SerialPortClaims.cpp` (`oeabt_serial` archive)
**Tests:** `backend.serial_startup_race` (regression: failed on every seed
before), `backend.serial_sim`
**Related:** [[SerialBus]], [[AutofocusService]], [[DeviceDiscoveryService]],
[[SimulatedSerial]], [[SyringePumpService]], [[../architecture/AppBackend]]

## Why

Found by the simulated rig ([[SimulatedSerial]]). Two discovery paths open
serial adapters the operator is about to connect:

- **Startup nanopositioner probe.** `StartupDiscoveryCoordinator` probes every
  OEABT candidate, which the filter defines by USB identity as the WCH
  CH340/CH341 family. A CH340 USB-RS485 dongle carrying the pumps is one. Each
  probe holds the port about 1 s (two identity commands, 500 ms timeouts) and
  writes `/1&\r` onto the RS485 bus. A pump Connect in that window failed
  with ACCESS_DENIED on Windows (on Linux it silently shared the tty), on
  every seed. The PL instrument had turned serial discovery off for this
  reason (`plScienceSerialBlockReason`).
- **Pulse-generator `auto` sweep** ([[PulseGeneratorService]]
  `discoverLiveView`, illuminated live view on port `"auto"`). It opens
  **every USB adapter whatever its chip** at 9600 baud. While it held an
  adapter, a pump Connect at 115200 failed with "different settings", and a
  nanopositioner connect failed busy. 14 of 33 race runs failed across the
  chip matrix before the fix.

## Rules

| Holder | API | Effect |
|---|---|---|
| Bus session ([[SerialBus]] `acquire`) | `claimForService(…, busSession=true)` | Registered first, then waits up to `kProbeYield` (3 s), unlocked, for another thread's probe on the port; kept for the session's life if it opens one |
| Connected nanopositioner ([[AutofocusService]] `connect`) | `claimForService` | Same; released in `disconnect()` |
| Nanopositioner probe (`probeNanopositionerEndpoint`) | `tryProbe` | Refused (probe fails, logged "skips …") when a service or another probe holds the port, or it is reserved; the startup retries take it later |
| Generator `auto` sweep (`discoverLiveView`) | `isReserved`, `tryProbe(…, kProbeYield, &holder)` | Skips reserved ports; waits for another probe; shares a live bus session; joins a bus session another client is opening (waits for it to exist, never opens its own in that window); a port an exclusive service holds is "busy" |
| config.json `pump_sample_com_port` / `pump_sheath_com_port` | `setReserved` (from `AppBackend::setLastConfigJson`) | Probes and the sweep skip `COM<n>` even before the pumps connect |

A service registers its claim **before** it waits. Without that, probes
retrying back to back (startup attempts, the sweep) could take the port in
turns and starve it past the 3 s bound. A probe never waits for itself: the
sweep holds a probe claim and opens the port through the bus layer on the
same thread. `backend.serial_sim` checks that a failed probe releases the
port in less than `kProbeYield` (it measures about 1 s).

The registry is never destroyed. A bus I/O thread that [[SerialBus]] detached
after a stalled port call releases its claim when the call returns, which can
be during process exit. A static registry would be destroyed under it (the
TSan lane caught this in `backend.syringe_pump_fake_serial`).

Ports are keyed like the bus layer (`key()`: basename, lowercase;
`SerialBusManager::normalizeKey` delegates to it).

## Chips

The rules never look at the chip. `backend.serial_startup_race` runs every
bridge in `serialsim::commonUsbSerialChips()` as the pump adapter and as the
generator adapter, rotated as the nanopositioner's. The catalog covers WCH
CH340/CH341/CH343/CH9102, FTDI FT232R/FT231X/FT232H/FT2232H/FT4232H, Silicon
Labs CP2102/CP2105/CP2108, Prolific PL2303/PL2303GC, Microchip MCP2221,
CDC-ACM (STM32 VCP, Arduino Uno, RP2040) and an onboard UART. The matrix
also runs with and without the pump port in config.json, with the sweep
racing startup and the operator. Chip-dependent behaviour is all upstream of
the claims:

- **OEABT candidates.** The nanopositioner filter lists CH340/CH341. The
  controller's chip is not known for every unit, so discovery probes in
  stages ([[DeviceDiscoveryService]] provider): listed chips and the saved
  endpoint first, then every other USB adapter, then ports without USB
  identity. Later stages run only when nothing earlier identified, and only
  on the job's last attempt, so a late-enumerating CH34x controller never
  causes other adapters to be probed. A controller on any catalog chip is
  connected at startup (`backend.serial_startup_race`).
- **The sweep.** It considers USB adapters only (VID/PID present). An
  onboard UART is never swept.

## Limits

- In-process only. A port held by another process, including a handle a
  crashed or hung instance never closed (#417), still fails at open with the
  OS code. The bus reports it as `PortBusy`, promptly. The startup
  coordinator's retries pick the port up once it is released
  (`backend.serial_startup_race` stale-holder scenarios).
- An unreserved, not-yet-connected adapter can still receive a probe's or
  the sweep's frames: an ASCII identity command, or a Modbus read at 9600.
  Modbus RTU slaves discard ASCII (bad CRC) and do not hear 9600 on a
  115200 bus. A Connect waits for the probe, so nothing fails. Naming the
  pump port in config.json keeps both off the pump bus entirely.
- The pulse-generator port is not in config.json. The startup probe can
  reach it when it is a CH34x and not yet connected, like an unconfigured
  pump bus.
