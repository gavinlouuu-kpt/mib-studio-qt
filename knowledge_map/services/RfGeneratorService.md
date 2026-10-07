# RfGeneratorService

> SCPI readback/provenance link to the **SIGLENT SSG3021X** RF signal
> generator that turns the sort TTL edge into the RF sort burst (pulse
> modulation, external trigger). Control and provenance only — **timing never
> goes over this link**; the pulse itself is [[TriggerService]]'s TTL edge.

**Source:** `src/backend/services/RfGeneratorService.cpp`,
`include/backend/services/RfGeneratorService.h`; transports
`include/backend/services/ScpiTransport.h`, `src/backend/services/ScpiTransportTcp.cpp`
(LAN, all platforms, also the factory), `ScpiTransportUsbtmcPosix.cpp` (Linux
`/dev/usbtmcN`), `ScpiTransportVisaWin32.cpp` (Windows, NI-VISA loaded at
runtime), `ScpiTransportUsbtmcStub.cpp` (elsewhere); provenance struct
`include/backend/recording/RfGeneratorProvenance.h` (portable).
**Tests:** `tests/backend/rf_generator_service_test.cpp`
(`backend.rf_generator_service`: identify-first, readback, preflight,
verified writes, link faults, real LAN transport over a loopback fake),
`tests/backend/experiment_readiness_test.cpp` (`rf.generator` gate),
`tests/recording/trigger_alignment_roundtrip_test.cpp` (provenance
round-trip + faults). Fake instrument: `tests/support/fake_ssg.h`.
**Related:** [[TriggerService]], [[PulseGeneratorService]] (the *other*
generator: camera ext-trigger, Modbus), [[../architecture/ExperimentCoordinator]],
[[../data-model/HDF5-Storage]], exec plan
`docs/exec-plans/active/2026-09-30-trigger-frame-alignment.md`.

## Signal chain and why this exists

```
grabber TTLIO12 / OUT2 ──► SSG TRIG IN ──(PULM:DELay)──► RF burst (PULM:WIDTh) ──► sorter
                                       └─ PULSE OUT: TTL envelope (loopback candidate)
```

The generator's trigger delay and pulse width decide which exposures the RF
burst overlaps, and its mode decides whether a TTL edge becomes a burst at
all. None of that was in the data file. This service reads it back and
[[../architecture/ExperimentCoordinator]] stores it as `rf_generator_*`
attributes so the file says what the sorter actually did.

## Responsibility (readback first, never write unasked)

- `setConfig(Config{enabled, transport "usb"|"lan", resource, timeoutMs})` —
  from the `rf_generator` block of the application config JSON
  ([[../architecture/AppBackend]]`::setLastConfigJson`):
  `{"rf_generator":{"enabled":true,"transport":"usb","resource":"auto","timeout_ms":1000}}`.
  The bundled `resources/defaults/config.json` ships the block **disabled**
  (`"transport":"lan","resource":""`); the default-merge in
  `AppConfigWatcher` adds it to existing user configs on upgrade, and the
  Config tab shows it as an editable `rf_generator` section (generic
  flattened-JSON tables — no dedicated UI). To enable on a rig: set
  `enabled` true and `resource` to `"<SSG IP>:5025"` (SSG: System >
  Interface > LAN). Enabled with an empty LAN address reports
  NotConfigured with that instruction instead of a failed connect.
  `resource`: `"auto"` (first USBTMC instrument), a VISA string
  (`USB0::0xF4EC::…::INSTR`), a device path (`/dev/usbtmc0`), or
  `host[:port]` (LAN raw socket, default 5025).
- `connect()` — opens the link, sends `*IDN?` **before anything else** and
  refuses (IncompatibleDevice) unless the reply is a Siglent SSG
  (`Siglent Technologies,SSG3021X,<serial>,<fw>`; the SSG5000X family shares
  the PULM set), then does a full readback. `ensureConnected(retryAfter)`
  backs off 5 s after a failure so readiness polls do not hammer a missing
  instrument. `disconnect()` leaves the instrument untouched.
- `readState()` → `RfGeneratorProvenance`: `:OUTPut?`, `:PULM:STATe?`,
  `:PULM:SOURce?`, `:PULM:MODE?`, `:PULM:TRIGger:MODE?`,
  `:PULM:TRIGger:EXTernal:SLOPe?`, `:PULM:DELay?`, `:PULM:WIDTh?`,
  `:PULM:PERiod?`, `:PULM:OUT:STATe?`, `:FREQuency?`, `:POWer?` (SSG3000X
  Series Programming Guide §3.3, §3.4.3, §3.4.4, §3.4.8; replies `1|0`,
  long-form enumerations, floats in s / Hz / dBm; a trailing unit is
  tolerated, anything else is ProtocolError).
- `preflightForSorting(state)` (pure) — blocking: RF output off, pulse
  modulation off, trigger mode not `EXTernal`, pulse source not `INTernal`,
  zero width; warning: PULSE OUT off (no envelope to loop back). Each issue
  carries the menu path and SCPI remedy.
- `applySortWindow(delayS, widthS)` — the one write path: range-checked
  (140 ns–300 s / 20 ns–300 s), `:PULM:DELay` + `:PULM:WIDTh`, `*OPC?`, then
  readback of both; a silently clamped value is VerifyMismatch and the
  cached state is not updated.
- `lastError()` / `lastErrorMessage()` — typed `LinkError` (NotConfigured,
  TransportUnavailable, OpenFailed, Timeout, ProtocolError,
  IncompatibleDevice, NotConnected, VerifyMismatch).

## Transports

`IScpiTransport` is "write a line, read a line" so the service is
transport-agnostic and tests drive it with an in-memory fake and a loopback
TCP server. `makeScpiTransport("lan"|"usb")` is the default factory; the
constructor takes another for tests.

- **LAN** (`ScpiTransportTcp.cpp`): raw socket, TCP_NODELAY, line assembly
  across partial reads, timeout via poll/select, peer close reported.
- **USB on Windows** (`ScpiTransportVisaWin32.cpp`): NI-VISA `visa64.dll`
  (fallback `visa32.dll`) loaded with `LoadLibrary`, so the build has no VISA
  SDK dependency; without VISA `open()` fails with "NI-VISA runtime is not
  installed". `auto` = first `USB?*INSTR` from `viFindRsrc`. Termchar `\n`.
  Not compiled in the Linux CI lanes; compiles clean under MSVC
  (rig PC, 2026-10-01). **Never run against an instrument** — the rig uses
  LAN and has no NI-VISA.
- **USB on Linux** (`ScpiTransportUsbtmcPosix.cpp`): kernel usbtmc class
  driver, `/dev/usbtmcN`, one message per read/write.

## Wiring

[[../architecture/AppBackend]] owns it (`rfGenerator()`), applies the config
block, disconnects at shutdown. [[../architecture/ExperimentCoordinator]]
readiness: gate `rf.generator` — NotRequired when sorting is off or
`rf_generator.enabled` is false or absent (including the bundled default
config); Fail when configured but down (reason = link error) or mis-armed
(reason lists the blocking preflight issues); Pass with identity + trigger
mode + window in the detail. The candidate `RunConfigurationSnapshot`
carries `rfGenerator*` fields (also in the stored run snapshot JSON). At
finalize the last readback is written with
`Hdf5Service::writeRfGeneratorProvenance`.

## Gotchas

- The instrument is **not re-queried at stop**: the provenance is the
  readback the readiness evaluation verified. A knob turned mid-run is not
  captured (a mid-run readback would need a poll and is not worth a SCPI
  round trip on the acquisition PC yet).
- The user manual disagrees with itself on which rear BNC takes the
  external trigger in Ext-Trig mode (§rear panel: TRIG IN/OUT; §8.4.4.13:
  PULSE IN/OUT, with Trigger Out auto-off). If PULSE IN/OUT is the trigger
  input, PULSE OUT is not available for the loopback and the T-off-the-TTL
  option applies — see the exec plan.
- Enumerations compare long- or short-form (`EXTernal` == `EXT`); the
  stored strings are whatever the firmware returned.
- USB and LAN are ~ms round trips with jitter: never derive a timestamp from
  a SCPI reply.
- The default-merge only reaches the app-managed `include/config.json`. A
  rig that runs from a **profile** (QSettings `Config/ExternalAppConfigPath`)
  never gets the `rf_generator` block, and the Config tab cannot add a
  section that is not in the file: add the block to the profile's
  `config.json` by hand (rig PC 2026-10-01, TD-21).
- LAN: the PC needs its own address on the SSG's subnet on the adapter the
  cable is in. A missing address and a wrong IP give the same gate text
  (`connect … timed out after 2000 ms`). On the rig PC the SSG
  (10.11.13.220, static) is on `Ethernet 3`; see
  `docs/evidence/2026-10-01-ssg-lan-link/README.md`.
