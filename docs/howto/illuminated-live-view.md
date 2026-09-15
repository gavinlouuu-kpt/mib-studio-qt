# One-click illuminated Live View (MindVision XGC + R5D)

Implemented for [issue #413](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/413).

## Everyday operation

For the default XGC + R5D rig, open MIB Studio Qt and press **Start Live View**.
A single discovered camera is selected automatically. Generator discovery and
the bundled preset require no manual setup. If multiple cameras are present,
select the intended MindVision camera first.
The application connects the generator, applies the saved camera setup,
arms OUT1 strobe, then enables the trigger train. **Stop** shuts down the
trigger train and OUT1 before releasing capture. No separate Apply to Camera,
generator Connect, Set or Start is necessary. Opening the application alone
never starts illumination.

Exposure remains visible in **Config → Camera trigger & strobe (MindVision)**.
To change it, stop Live View, edit exposure and Save, then press Play again.
The low-level timing and serial controls are collapsed under
**Advanced — Hardware Setup**. Unsaved edits are not applied by Play.

## Custom or ambiguous hardware setup (optional)

1. Select the MindVision camera in Connect.
2. In the MindVision configuration tab, expand **Advanced — Hardware Setup**.
3. Select the generator's serial port, baud/framing, Modbus address and channel.
   Do not guess an adapter when several are connected.
4. Click **Use XGC + R5D preset for Live View**. This saves the chosen connection
   and the tested starting settings below; it does not operate hardware.
5. Collapse Advanced. Press Preview Play; subsequently use only Play and Stop.

Saved values live in the active `mindvisionConfig.json` shown in the tab, not
in a second hidden timing preset. Reopening the app loads that file for the
next capture. Switching to mock or another camera does not operate the rig.
With `port: "auto"`, the adapter is rediscovered at each start. Explicit ports
require updating Hardware Setup if the device node changes. Automatic discovery
uses read-only probes and requires exactly one compatible generator before
enabling the configured channel; it never selects among multiple matches.

The preset uses 512×96 ROI, manual 100 µs exposure, external rising-edge trigger
(mode 2, signal type 0: one frame per generator pulse), zero acquisition
delay/jitter, manual strobe (mode 1) width 100 µs, delay 0, polarity 0 (on this
rig polarity 0 pulses OUT1 with the strobe; polarity 1 left OUT1 idling high and
the LED on — measured 2026-09-15). Generator channel is the selected channel
(normally channel 1), 1000 Hz and 2% duty (a 20 µs trigger pulse). Serial
connection settings come from the selected controls, not hard-coded host paths.

The default JSON includes this section (`auto` discovers the generator; custom
rigs may use an explicit port):

```json
"live_view": {
  "enabled": true,
  "port": "auto",
  "baud": 9600,
  "data_bits": 8,
  "parity": "N",
  "stop_bits": 1,
  "address": 1,
  "channel": 1,
  "frequency_hz": 1000.0,
  "duty_percent": 2.0
}
```

Existing profiles without this section retain manual generator operation.
Set `enabled` to false, save while stopped, to return to manual operation.
The preset deliberately does not replace existing installations' defaults or
enable a device that has not been selected during hardware setup.

## Backend lifecycle

- `AppBackend::stageMindVisionConfigFromFile` validates/stages the next-start
  profile without acquiring the device. All MindVision factory paths create
  the same capture-owned illumination session from that profile.
- The capture worker connects through `PulseGeneratorService` and the existing
  shared `SerialBusManager`. It gates the selected channel off, sets frequency
  and configured duty, and checks Modbus register readback.
- `MindVisionCamera` applies the profile on its own SDK handle. For illuminated
  profiles a failed setter is fatal. After CameraPlay, it configures input 0 as
  trigger and output 0 as strobe, standard shutter, then checks exposure,
  auto-exposure, trigger and strobe readback before enabling the generator.
- Generator enable also checks register readback. Manual generator writes and
  disconnect are refused while a capture generation owns the generator.
  Physical OUT1 (SDK index 0) is illumination; OUT2 (index 1) remains sorting.
- Stop first gates the generator off and reads it back; then forces OUT1 to
  GPIO low and stops the SDK. Partial start failures use the same cleanup.
  Retrieval errors and a three-second no-frame timeout fault illuminated runs.
- An unconfirmed generator/LED stop survives in the lifecycle failure record
  and is surfaced by the Stop action. OFF is never inferred from a failed write.
  Manual control becomes available for connection repair and an explicit Stop.
- No per-frame serial traffic, timer-based strobe scheduling or second camera
  handle is introduced. Mock/free-run/manual profiles retain their behavior.

A successful Modbus/SDK acknowledgement is configuration evidence, **not a
measurement of physical LED current**. As with any software shutdown, loss of
power/control connectivity can prevent confirmation; do not claim OFF then.

## Commissioning evidence and limits

September 10 bench evidence for the preset: Rigol CH2 measured 5 kHz,
200 µs period and about 20 µs trigger width. CH1 measured LED-current pulses
at 5 kHz with **64.6 µs** duration. Settled image mean was about **150/255**,
versus **5.6/255** with LED off. Startup buffers were drained for 1.2 seconds
before those image comparisons. This supports useful illumination overlap.

A 100 µs **strobe command is not 100 µs LED current**. The older calibration
measured approximately 65–68 µs current for that command on this bench.
Exact sensor exposure edges remain unmeasured. The earlier inferred
47 µs exposure-start/zero-overlap claim is not an established calibration and
must not be used to derive acquisition delay. Delay sweeps and settled-frame
checks on September 10 contradicted treating that estimate as exact.

Rigol waveform validation belongs in commissioning and after timing/wiring
changes, not in the normal Play workflow. Validate CH2 trigger and CH1 actual
current frequency/period/width. Image brightness and SDK status are supplementary.
The automated tests in this change exercise fake SDK/Modbus devices; they do
not constitute a new hardware timing acceptance run or an optical calibration.

## Connection troubleshooting

Discovery is not proof that acquisition traffic reaches the camera. On
September 10, CameraInit error −14 was caused by a Tailscale exit-node route
intercepting the camera's dedicated Ethernet path, not another camera owner.
A persistent camera-IP-only host routing exception restored access without a
power cycle. Check route selection as well as competing SDK processes when
this recurs. Qt must not silently change privileged routing or unrelated VPN
routes; routing repair is host setup, separate from Live View start.

## Verification

`backend.illuminated_live` covers missing setup, startup ordering, configuration
and readback failures, generator enable/stop failure, idempotent stop, pipeline
fault cleanup, frame/buffer accounting, and repeated cross-thread ownership.
`frontend.config_tabs_state` checks collapsed advanced controls, exposure
visibility, saved preset/adapter persistence, unrelated-key preservation and
reopening without Apply. Existing lifecycle, conversion and live-view latency
checks remain applicable. A real-rig acceptance run is still required for the
changed desktop build before labeling that build hardware-validated.


### Everyday FPS adjustment

Requested FPS is visible beside Exposure for a saved illuminated rig. Stop capture,
change FPS, Save, and Play to apply it through the coordinated generator startup.
It edits `live_view.frequency_hz`, not the camera's free-running speed selector.
The generator supports 400–40000 Hz; this is not a camera throughput guarantee.
The rig default is 1000 FPS at 512×96 (rising-edge trigger, measured 997.7
frames/s on 2026-09-15); with edge trigger the camera tops out near 4500
frames/s at this ROI, so requests above that are not honoured. Observe actual acquisition rate and
use Rigol for physical timing acceptance when commissioning another rate.

Changing FPS preserves the trigger's active duration by scaling saved duty with
frequency: the preset's 1000 Hz / 2% becomes 2500 Hz / 5%, retaining a requested
20 µs trigger pulse. Exposure and strobe width/delay are not silently changed.
Existing backend validation rejects exposure or strobe timing that exceeds the
new period, and invalid generator duty. Legacy/manual profiles leave FPS disabled.
The real-widget regression covers visibility, persistence, duty compensation,
unchanged exposure/strobe, and restoration after reopening.


### Separate setup from raw configuration

Normal MindVision use hides the config file path and raw editor. Opening Hardware
Setup shows the setting form without also showing JSON; an explicit “Edit raw
configuration (JSON)” toggle reveals the editor. Closing Hardware Setup closes
that editor too, without discarding edits. Saved illuminated rigs describe Save
as staging the next Play, not requiring a separate Apply to Camera operation.


### Automatic default rig setup (September 14 follow-up)

The bundled XGC/R5D profile now enables illuminated Live View with `port: "auto"`,
9600 8N1, address 1, channel 1, 1000 Hz / 2% (20 µs pulse), exposure 100 µs,
rising-edge external trigger and manual strobe 100 µs / zero delay with
polarity 0 (the setting that pulses OUT1 on this rig; see the September 15
measurements). The existing
single-camera discovery selects the camera; Start performs read-only discovery
of USB serial adapters at the configured address on the capture worker. Exactly
one generator-compatible response is required before normal gated startup.
No match or multiple matches produces a specific error; no output is enabled by
discovery. Channel/wiring cannot be discovered electronically: channel 1 is the
known rig preset, not an inferred connection. Custom address/serial/wiring uses
Hardware Setup as an exception. Auto mode re-discovers the adapter each start,
so port renumbering does not require manually saving a new path.

Fresh installs save the bundled profile automatically. Only a byte-structure-
equivalent historical bundled JSON profile at the default path is upgraded;
custom and external profiles are preserved. Explicit saved ports continue to
work unchanged. Discovery exceptions are recorded as camera startup failures
and pass through illumination cleanup. The earlier mandatory one-time manual
setup instructions apply only to custom or ambiguous rigs, not the default rig.
Hardware acceptance of this changed build remains outstanding.


### Review follow-up on the rig PC (September 14, second pass)

A read-only Modbus identity probe (FC03, address 1, 9600 8N1) of the rig PC's
five USB serial ports found two things the first implementation would have
mishandled. COM4 answered with twelve zeroed registers and, on every further
probe (registers past the map, pump registers), behaved exactly like the
generator on COM6 — it is a second, never-configured pulse generator module.
The lenient "plausible generator" rule accepted it, so automatic discovery
would have been ambiguous, or would have adopted COM4 and written frequency and
duty into it while the real generator was unavailable. COM6, the generator's
adapter, was held open by the running installed application, which reports as
"port busy" rather than "no device".

Changes made in response:

- **Adoption keyed on the requested channel.** `port: "auto"` discovery
  counts a port only when the identity read has the generator shape, the
  requested channel (channel 1 for the preset) holds a non-zero frequency
  inside the module's 400–40000 Hz range (`identityChannelConfigured`), and a
  read of the dLSP syringe pump's syringe-volume register (0x0061) answers
  zero. The module keeps 0 Hz for channels it has never set: on the rig the
  generator reads channel 1 = 5000 Hz, channels 2–4 = 0 Hz, so a rule that
  demanded all four channels would have rejected the real device (it did,
  in the first cut of this pass, before the probe caught it). A module whose
  requested channel was never set is reported with the remedy ("set it once
  in Hardware Setup"); a pump left channel-enabled at address 1 has the
  generator shape on channel 1 (raw 65536 = 655.36 Hz) and is excluded by the
  syringe-volume read, because the generator answers 0 for any register
  outside its map. The manual Scan in Hardware Setup keeps the lenient rule
  because the operator chooses the port there. Discovery still never writes.
- **Actionable discovery errors.** No match now names adapters held by another
  program, modules whose requested channel is unset, and devices that answered
  but are not a generator; several matches name them all. The port recorded is
  the system name (`COM6`, `ttyUSB0`), the same form Hardware Setup saves.
- **One timing rule set.** The `live_view` block is parsed and validated by
  `parseConfig` (port, address 1–247, channel 1–4, 400–40000 Hz, duty in
  (0, 100), serial framing) together with the period rules: exposure must not
  exceed the trigger period, strobe delay + width must be shorter than it, and
  the trigger pulse must be at least 1 µs. Messages state the period, for
  example "Requested FPS 40000 gives a 25 us trigger period; exposure 100 us
  must not exceed it". The capture factory and **Save** use the same parse, so
  an FPS that cannot fit the saved exposure and strobe is refused at Save with
  the file untouched instead of failing at Play. With the preset's 100 µs
  exposure and strobe the maximum is just under 10000 FPS; the camera's readout
  time decides the achievable rate below that.
- **Cancellation.** A Stop issued while discovery or generator preparation is
  running is honoured before the SDK handle is opened, and again before
  CameraPlay, not only after arming.
- **Shutdown record.** An unconfirmed generator or LED OFF now survives a later
  handle-drain fault in the same Stop and says which OFF failed.
- **Arm readback diagnostics.** Each mismatched camera setting is logged with
  its read-back value. Exposure readback tolerates the sensor's line-time
  quantization (5 %, at least 1 µs) and logs the actual value; other settings
  must match exactly.
- **Default migration.** The upgrade compares the file against a verbatim copy
  of the pre-#413 bundled profile (`ConfigTabs::upgradedMindVisionDefault`),
  which is unit-tested. The rig PC's current default profile is byte-equivalent
  to that historical profile, so it will be upgraded on first start of this
  build; an edited profile at the same path is preserved.
- **FPS control.** The Requested FPS box commits on Enter or focus-out, and the
  compensated duty is rounded to the generator's 0.01 % resolution.

The camera was not attached to the rig PC during this pass and the PC has no
C++ toolchain, so this build has not run on the rig. The probe above is
discovery evidence only; it is not a timing measurement.

Update, September 15: a toolchain was installed on the rig PC (MSVC 2022
Build Tools, CMake, Ninja, Conan; dependencies built from source) and this
branch builds there with the installed MindVision 2.1.10.195 platform. The
Windows fast test lane passes (102 tests), including `frontend.config_tabs_state`
and `backend.illuminated_live`, which no PR lane compiles. With the installed
app closed, the generator on COM6 reads channel 1 = 5000 Hz / 50 % and
channels 2–4 = 0 Hz; the SDK enumerates one MV-XG51GM (GigE).

Rig run records for September 15 (frame rates per trigger mode, the polarity
finding and the new default): [evidence](../evidence/2026-09-15-illuminated-live-rig/README.md).
