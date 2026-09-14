# One-click illuminated Live View (MindVision XGC + R5D)

Implemented for [issue #413](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/413).

## Everyday operation

After the rig has been configured once, open MIB Studio Qt, select the saved
MindVision camera if it is not auto-selected, and press **Preview → Play**.
The application connects the generator, applies the saved camera setup,
arms OUT1 strobe, then enables the trigger train. **Stop** shuts down the
trigger train and OUT1 before releasing capture. No separate Apply to Camera,
generator Connect, Set or Start is necessary. Opening the application alone
never starts illumination.

Exposure remains visible in **Config → Camera trigger & strobe (MindVision)**.
To change it, stop Live View, edit exposure and Save, then press Play again.
The low-level timing and serial controls are collapsed under
**Advanced — Hardware Setup**. Unsaved edits are not applied by Play.

## One-time hardware setup

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
Changing the adapter/device node requires updating Hardware Setup. The app
never scans and writes into an arbitrary generator automatically.

The preset uses 512×96 ROI, manual 100 µs exposure, external high-level trigger
(mode 2, signal type 2), one frame per trigger, zero acquisition delay/jitter,
manual active-high strobe (mode 1), width 100 µs and delay 0. Generator channel
is the selected channel (normally channel 1), 5000 Hz and 10% duty. Serial
connection settings come from the selected controls, not hard-coded host paths.

The JSON adds this section (port is an example, replace with the chosen port):

```json
"live_view": {
  "enabled": true,
  "port": "COM3",
  "baud": 9600,
  "data_bits": 8,
  "parity": "N",
  "stop_bits": 1,
  "address": 1,
  "channel": 1,
  "frequency_hz": 5000.0,
  "duty_percent": 10.0
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
