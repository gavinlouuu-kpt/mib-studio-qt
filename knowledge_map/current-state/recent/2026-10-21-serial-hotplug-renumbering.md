## 2026-10-21 — Serial hot-plug, renumbered pump adapters, sccache fallback

Follow-up to [[2026-10-19-serial-sim-startup-race]], closing the limits it
left open.

- **Hot-plug.** `StartupDiscoveryCoordinator` watches the serial ports
  (`Hooks::listSerialPorts`). After a nanopositioner step ends without a
  connection, a port that appears reruns it, so a controller plugged in after
  the startup retries is connected. Not while one is connected or the operator
  must choose. [[services/DeviceDiscoveryService]]
- **Renumbered pump adapters.** `SerialAdapterIdentity.h`: config.json
  remembers `pump_*_adapter` after a connect; an adapter with a serial number
  is followed to its new COM port and reserved there; a port that now carries
  another adapter is refused; a CH340 (no serial) is never guessed.
  [[frontend/SyringePumpTab]], [[services/SerialPortClaims]]
- **CI.** `.github/actions/start-sccache` starts sccache on a local disk cache
  when the Actions cache refuses (503 ServerBusy), instead of failing the
  build. [[build-and-run/Build]]

Tests: `backend.serial_startup_race` (hot-plug scenario, CH340 and CP2102),
`backend.serial_adapter_identity` (new).
