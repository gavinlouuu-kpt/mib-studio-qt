# OEABT Single-Piezo Nanopositioner

MIB Studio implements OEABT compatibility as a user-space serial backend. The
controller's WCH CH341 USB bridge is already supported by Linux's `ch341`
kernel driver; no vendor kernel module is installed or required.

## Interoperability evidence

The compatibility layer was derived from static inspection of the unsigned
`oeabt-single-piezo-setup-1.5.0.exe` installer and its `SinglePiezo.exe`
payload. Neither binary is redistributed or loaded by MIB Studio.

- Installer SHA-256:
  `25c96f93df27fee96286f5eaca6249e34e9b7f12af4dcbae8ec9cd4fb215d687`
- Application SHA-256:
  `8d5ebb9a392d6532e1881eb2036303f15952f99c8f45e236d2a4a84376e8dee9`
- OEABT O'motion command-manual SHA-256:
  `2c05713b578f5f2cbaf9e6b42ad0acd3621ff6a9b9b04025fcf41effd2537b32`
- Serial settings: 115200 baud, 8 data bits, no parity, one stop bit, no
  flow control.
- Commands end in carriage return. Responses are line-oriented with a 500 ms
  vendor timeout.

The official [SinglePiezo V1.5.0 manual](https://docs.oeabt.com/docs/c4ef6c62-aabe-4acd-afd4-faa6463cd9d6/)
independently confirms that device discovery probes each available serial port,
that a port is listed only after the expected identification response, and that
an opened device uses 115200 baud, 8 data bits, one stop bit, and no parity. It
does not publish the command grammar or response framing.

| Purpose | Command |
|---|---|
| Identify | `/1&\r` (SinglePiezo 1.5.0), then `/1&R\r` (command manual fallback) |
| Read voltage | `/1?aVtR\r` |
| Read maximum voltage | `/1?aVtmR\r` |
| Read maximum stroke | `/1?aSR\r` |
| Query analog/PWM support | `/1?et1R\r`, `/1?et2R\r` |
| Select manual/analog/PWM mode | `/1aM1et0R\r`, `/1aM1et1R\r`, `/1aM1et2R\r` |
| Set voltage in integer millivolts | `/1aM1Vt<mV>R\r` |

The June 2026 OEABT O'motion command manual defines the response as a four-byte
header, payload, and three-byte trailer:

```text
FF 1F 30 <status> <payload bytes> 03 0D 0A
```

The manual calls header byte `1F` an ASCII `/`, although ASCII `/` is `2F`.
Physical V0.5.4 captures confirm `FF 2F 30 60`; the parser also accepts
the manual's `1F` spelling for compatibility. Status bit 6 must be set, bit 5 reports readiness, and
bits 3–0 report controller errors. All-axis voltage, maximum-voltage, stroke,
and position responses contain four comma-separated fields; `?et` capability
responses may be scalar on older firmware or four fields on V0.5.4. `/1?aAR` reports all positions in micrometres and
`/1?cnR` reports actuator connection state. The implementation validates the
framing and status but does not yet expose position or connection-state queries.

The supplied manual is retained with the hardware evidence at:

```text
/mnt/hdd/shared/projects/mib-studio-qt/windows-vm/oeabt-test-20260901/
  omotion-single-piezo-command-manual-20260629.pdf
```

## Linux setup

1. Connect USB and the controller's required DC supply.
2. Confirm Linux attached the bridge:

   ```bash
   ls -l /dev/serial/by-path
   lsmod | grep ch341
   ```

3. Ensure the operator belongs to `dialout`; log out and back in after adding
   membership.
4. Select the physical `/dev/serial/by-path/...` endpoint in MIB Studio when
   multiple generic CH341 adapters are present.

Do not install a generic `1a86:7523` udev naming rule. That VID/PID identifies
the USB bridge, not the controller, and multiple unrelated devices can share
it. MIB treats it only as a probe candidate and requires the response text
`Oeabt pzt controller` before declaring a connection.

## Diagnostic CLI

`oeabtctl` is built by default (`MIB_BUILD_OEABT_TOOLS=ON`):

```bash
oeabtctl list
oeabtctl probe --port /dev/serial/by-path/<controller>
oeabtctl status --port /dev/serial/by-path/<controller>
oeabtctl monitor --port /dev/serial/by-path/<controller>
```

These operations never change mode or voltage. Mutating operations require an
explicit interlock:

```bash
oeabtctl verify-write \
  --port /dev/serial/by-path/<controller> \
  --target-volts <operator-approved-safe-voltage> \
  --allow-write
```

`verify-write` identifies the device, reads its maximum and original voltage,
sets and reads back the target, then restores and reads back the original.
Never use it without an operator-approved mechanically safe target and direct
supervision.

## MIB Studio behavior

- Backend choices are Auto, OEABT, and CoreMOR. Auto probes only enumerated
  candidates and still requires protocol identity.
- Connect is observe-only. It reads identity, capabilities, limits, and the
  current voltage without applying the historical initial-voltage setting.
- The first explicit manual/autofocus voltage request selects manual mode and
  marks the session active.
- A normal disconnect applies the configured safe-shutdown voltage only after
  an active session. Disconnecting an observe-only session sends no write.
- Requested voltages must fit both configured safety limits and the
  controller-reported maximum; invalid values are rejected rather than
  clamped silently.

## Current hardware gate

On 2026-08-31, physical unplug/replug confirmed
`/dev/serial/by-path/pci-0000:00:14.0-usbv2-0:10:1.0-port0` as the OEABT CH341
adapter. With the controller reported powered, it returned no startup bytes
and no response to the exact identity command. No mode or voltage command was
sent.

On 2026-09-01, the same physical adapter was passed through to a disposable,
network-isolated Windows 11 VM. The OEABT SinglePiezo 1.5.0 installer left the
64-bit VM without a working USB-serial driver. Static extraction explains why:
although the bundled `drivers` directory contains an INF advertising AMD64
support, that INF references `CH341S64.sys`, `CH341PORTSA64.dll`, and
`CH341PTA64.dll`, none of which are present in the installer. This contradicts
the manual's statement that the bundled driver is installed automatically on
64-bit Windows. Installing the signed WCH 3.8.2023.2 AMD64 package from
Microsoft Update Catalog made the adapter enumerate as `USB-SERIAL CH340
(COM3)`. A host USB capture then proved that the vendor application configured
the adapter and transmitted `/1&\r` at 115200-8-N-1. No response payload was
returned, and the application's compatible-device list remained blank. The
Windows test sent no mode or voltage command. This reproduced the silent response independently of the Linux client.

After receiving the O'motion command manual, the Linux client also sent its
documented `/1&R\r` firmware query to the connected controller at 115200-8-N-1.
It likewise received zero bytes. The manual says controller parameters,
including a changed baud rate, return to defaults only after controller power
loss. A full controller/DC power cycle is therefore required before concluding
that its firmware or internal serial path is incompatible; reconnecting USB
alone may not reset a runtime baud-rate change.

The historical handshake failure below was resolved on September 10–11; see
connected-controller evidence. Normal precision-control acceptance still requires
`oeabtctl verify-write` with an operator-selected safe target and target/restore
readbacks within the unchanged 0.02 V tolerance. The observed low-voltage
mismatch does not pass that gate.

## Connected-controller evidence (2026-09-10 and 2026-09-11)

Both identity forms returned `Oeabt pzt controller V0.5.4` at 115200 8N1.
The controller reported a 100000 mV maximum and a 62 µm maximum stroke.
Analog/PWM capabilities were `0,0,0,0` and `1,0,0,0`. V0.5.4 emits unframed
`hal_pwm_io_set_vol_input_mode 0` diagnostics around mode acknowledgements;
the serial adapter extracts the framed reply across fragmented reads, with a
bounded timeout and size limit, instead of accepting the first newline.

User-authorized commands demonstrated communication, not displacement accuracy:

- 2 V: accepted; after approximately 12 seconds, six readbacks remained
  1.423–1.470 V; reported position was 1.24 µm. Returning the command to
  0.879 V yielded 0.791 V and a 0.54 µm reported position.
- 30 V → 0 V → 30 V: all commands acknowledged, three samples per step
  at approximately two-second intervals. First 30 V: 29.704–29.733 V.
  At zero: 4.881 V initially, then 0.846 and 0.872 V. Final 30 V:
  29.689–29.784 V. Reported positions: 18.60 → 0 → 18.60 µm.
- The final command on September 11 was 30 V in manual mode. This is historical
  evidence, not a claim about the present hardware state.
- Position equals commanded voltage × 62/100 and must be treated as a reference,
  not independently measured displacement. Low-voltage accuracy and physical
  displacement remain unverified; do not relax acceptance tolerances to hide it.

The silent-handshake blocker is resolved. Full voltage-accuracy/release acceptance
remains open. No new physical motion is needed to run the software regression
suite. Device numbering changed between sessions: enumerate and verify identity;
do not assume a remembered ttyUSB number still identifies this controller.

## Native backend integration

The implementation uses the existing POSIX/Win32 `ISerialPort` and native port
enumeration on current develop, with no Qt dependency in `mib_backend` or
`oeabtctl`. `SerialTransport` extracts complete frames despite fragmented debug
output. A mutex-backed backend proxy serializes complete multi-command operations
for concurrent callers; no dedicated Qt event-loop thread is needed.


### PR #413 discovery integration

The shared vendor registry now uses native nanopositioner endpoints and includes
both OEABT and CoreMorrow/XMT probes. Startup scans all candidates on its worker,
auto-connects only a unique validated match, and Refresh repeats discovery.
Connection and serial/vendor controls are disabled while scanning. A legacy
COM-only setting retains its port preference but defaults to automatic vendor
selection; an explicit saved vendor is preserved. Discovery and connection are
observe-only, with no voltage or mode writes.
