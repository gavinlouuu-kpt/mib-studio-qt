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
pump port for ~2 s per attempt and wrote ASCII onto the bus, so a pump Connect in that window
failed busy. Fixed with [[services/SerialPortClaims]]: probes skip ports a bus session or the
connected nanopositioner holds, and ports config.json assigns to the pumps. A service waits up
to 3 s for a probe on its port instead of failing.

Guards: `backend.serial_sim` (the rig, services through it, scenario/env switch) and
`backend.serial_startup_race`. The race test runs the real coordinator against seeded operator
connects, with and without a configured pump port. It also covers a stale process holding the
nanopositioner port (startup retries, then connects) and one holding the pump bus (Connect
fails promptly as busy, then succeeds once released). Teardown without `shutdown()` releases
every port. See [[services/SerialBus]], [[services/DeviceDiscoveryService]],
[[services/AutofocusService]], [[architecture/AppBackend]].
