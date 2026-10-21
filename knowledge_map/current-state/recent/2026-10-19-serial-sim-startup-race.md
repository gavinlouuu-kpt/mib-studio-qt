## 2026-10-19 — Simulated serial hardware; startup probe no longer blocks the pumps

Gavin asked for a simulated tool so startup COM-port races show up before the rig.
[[services/SimulatedSerial]] replaces every COM port in the process with fake ports and
devices (dLSP/Tushui pumps, pulse generator, ZC300, OEABT). Opens are exclusive on Linux
too, enumeration and replies are late and seeded-jittered, devices can be unplugged, and a
stale process can hold a port. A ledger records every open, refusal and stray frame. Tests
install it in code. The app uses it with `MIB_SERIAL_MODE=sim` (+ `MIB_SERIAL_SIM_SCENARIO`,
`MIB_SERIAL_SIM_SEED`); guide in `docs/howto/simulated-serial.md`.

On its first run it reproduced a startup race on every seed. The nanopositioner probe treats
any CH340 adapter as an OEABT candidate, including a CH340 USB-RS485 pump dongle. It held the
pump port about 1 s per attempt and wrote ASCII onto the bus, so a pump Connect in that window
failed busy. Gavin then asked for chips other than the CH340. The rig gained a catalog of the
common USB-serial bridges (WCH, FTDI, Silicon Labs, Prolific, Microchip, CDC-ACM boards,
onboard UART). The chip matrix found a second, chip-independent race: the pulse generator's
`auto` sweep (illuminated live view) opens every USB adapter at 9600 baud, so a pump Connect
at 115200 failed with "different settings" (14 of 33 runs).

Fixed with [[services/SerialPortClaims]]:
- Probes and the sweep skip ports a bus session or the connected nanopositioner holds, and
  ports config.json assigns to the pumps.
- A service registers and then waits up to 3 s for a probe on its port instead of failing, so
  back-to-back probes cannot starve it.
- The sweep joins a bus session another client is opening instead of racing it.

The controller's bridge chip is not known for every unit, so nanopositioner discovery no longer
stops at the CH340/CH341 filter. It probes the saved endpoint and the listed chips first. On the
job's last attempt, and only if nothing identified, it also probes every other USB adapter and
then ports without USB identity (see [[services/DeviceDiscoveryService]]).

Guards: `backend.serial_sim` (the rig, services through it, scenario/env switch) and
`backend.serial_startup_race`. The race test runs the real coordinator against seeded operator
connects, with and without a configured pump port, across every chip in the catalog for the
pump and generator adapters, with the sweep racing them. It also covers a stale process holding the
nanopositioner port (startup retries, then connects) and one holding the pump bus (Connect
fails promptly as busy, then succeeds once released). Teardown without `shutdown()` releases
every port. See [[services/SerialBus]], [[services/DeviceDiscoveryService]],
[[services/AutofocusService]], [[architecture/AppBackend]].
