# OEABT nanopositioner compatibility

## Outcome

Added a clean-room OEABT protocol core, Qt serial adapter, `oeabtctl`, and a
backend-neutral autofocus integration for Linux and Windows. CoreMOR remains a
separate Windows backend. Discovery and connection are read-only; voltage
writes require explicit control and normal disconnect applies safe voltage
only after an active session.

## Evidence and safety

- The vendor app uses Qt SerialPort over a standard WCH CH341 bridge at
  115200-8-N-1.
- The official SinglePiezo V1.5.0 manual independently confirms 115200-8-N-1
  and identity-response-gated enumeration, but does not publish the command
  grammar or response framing.
- The supplied June 2026 O'motion command manual now provides the grammar,
  response frame, readiness/error status bits, query units, and the documented
  `/1&R` firmware-query form. The client tries that read-only form after the
  captured SinglePiezo `/1&` form and strictly validates response framing and
  status. The manual inconsistently labels literal `1F` as ASCII `/` (`2F`), so
  both values are accepted pending a physical response capture.
- Linux already binds the hardware through `ch341`; this is a user-space
  compatibility layer, not a kernel driver.
- Physical unplug/replug confirmed `/dev/ttyUSB1` as the OEABT adapter, but it
  returned no bytes to the recovered identity query while powered.
- A disposable, network-isolated Windows 11 VM reproduced the result with the
  vendor SinglePiezo 1.5.0 application. The vendor installer contains a CH341
  driver package, as its manual claims, but its AMD64 INF references three
  64-bit driver files absent from the installer. After installing Microsoft's
  signed WCH 3.8.2023.2 driver, Windows enumerated the adapter as `USB-SERIAL
  CH340 (COM3)`. A host-side USB capture proved that the vendor app opened the
  port and transmitted the exact `/1&\r` identity request, but received no
  payload; the app consequently left its compatible-device list blank.
- The Windows test sent no voltage or mode command. Its capture is retained in
  the isolated VM artifact directory for protocol evidence.
- A subsequent connected-device Linux probe tried both the captured `/1&` and
  documented `/1&R` identity forms and received zero bytes from both. Because
  the command manual says runtime communication settings reset on controller
  power loss, repeat only after a full controller/DC power cycle before
  escalating firmware or internal-serial incompatibility to OEABT.
- Real-hardware write enablement remains gated on Linux identity/read and
  opt-in set/restore acceptance; the isolated Windows reference test is
  complete.

## Key implementation choices

- `oeabt_core` is Qt-free C++17 and depends on an injected byte transport.
- `oeabt_qt_serial` owns endpoint enumeration and QSerialPort transactions.
- Stable Linux selection prefers `/dev/serial/by-path` because generic CH341
  serial identifiers collide on this host.
- Legacy `autofocus_com_port` configuration migrates to an explicit CoreMOR
  endpoint; new configuration stores backend plus an opaque endpoint ID.

See `docs/integration/oeabt-nanopositioner.md` and
`docs/exec-plans/active/2026-08-31-oeabt-nanopositioner.md`.

## September 15 follow-up

September 10–11 live tests resolved the silent identity handshake (V0.5.4).
The integration guide records the 2 V and 30 V → 0 V → 30 V readbacks and
reference-position limitations. Regression tests first reproduced four-field
capability rejection and debug-line interference, then verified scalar/four-axis
compatibility and fragmented framed responses with diagnostic text. Voltage
accuracy and independently measured displacement remain open acceptance items.
