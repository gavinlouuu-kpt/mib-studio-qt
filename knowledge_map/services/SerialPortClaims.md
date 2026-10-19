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

Found by the simulated rig ([[SimulatedSerial]]) on its first run.
`StartupDiscoveryCoordinator` probes every CH340 adapter for an OEABT
nanopositioner (VID 1a86 is an OEABT candidate). A CH340 USB-RS485 dongle
carrying the pumps is one of them. The probe held the pump port for ~2 s per
attempt (two identity commands, 500 ms timeouts, one retry) and wrote
`/1&\r` onto the RS485 bus. A pump Connect in that window failed with
ACCESS_DENIED on Windows. On Linux it silently shared the tty. The PL
instrument had already turned serial discovery off for this reason
(`plScienceSerialBlockReason`). The desktop path still raced.

## Rules

| Holder | API | Effect |
|---|---|---|
| Bus session ([[SerialBus]] `acquire`, new session only) | `claimForService` | Waits up to `kProbeYield` (3 s), unlocked, for a probe on the port; held for the session's life |
| Connected nanopositioner ([[AutofocusService]] `connect`) | `claimForService` | Same; released in `disconnect()` |
| Nanopositioner probe (`probeNanopositionerEndpoint`) | `tryProbe` | Refused (probe fails, logged "skips …") when a service or another probe holds the port, or it is reserved |
| config.json `pump_sample_com_port` / `pump_sheath_com_port` | `setReserved` (from `AppBackend::setLastConfigJson`) | Probes skip `COM<n>` even before the pumps connect |

Ports are keyed like the bus layer (`key()`: basename, lowercase;
`SerialBusManager::normalizeKey` delegates to it).

## Limits

- In-process only. A port held by another process, including a handle a
  crashed or hung instance never closed (#417), still fails at open with the
  OS code. The bus reports it as `PortBusy`, promptly. The startup
  coordinator's retries pick the port up once it is released
  (`backend.serial_startup_race` stale-holder scenarios).
- With no pump port in config.json, the probe can still identify the RS485
  bus before the pumps connect: one ASCII frame per attempt reaches the bus.
  Modbus RTU slaves discard it (bad CRC), and a pump Connect waits for the
  probe, so nothing fails.
- The pulse-generator port is not in config.json. It is usually an FTDI
  adapter, not an OEABT candidate. If it is a CH340 and not yet connected,
  it gets the same treatment as an unconfigured pump bus.
