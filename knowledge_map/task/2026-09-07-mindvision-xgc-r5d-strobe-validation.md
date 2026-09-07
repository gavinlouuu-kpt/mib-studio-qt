# 2026-09-07 — MindVision XGC + R5D strobe validation

## Goal

Close hardware verification for the MV-XG51GM external-trigger and strobe
path using the Zhongsheng 5 kHz generator, R5D pulsed constant-current driver,
Rigol scope, and a blue LED.

## Validated topology

Zhongsheng channel 1 drives MindVision input 0. Physical camera OUT1 (SDK
output index 0) drives the R5D trigger input. Rigol CH2 monitors the
camera-trigger signal; CH1 monitors the low-side LED-current shunt.

The camera required external trigger mode 2 with high-level signal type 2.
Signal types 0 and 1 returned no frames. Type 2 captured 10/10 frames and a
five-second 1 µs exposure run returned 25,248 frames (about 5,049 fps) at a
512×96 ROI.

## Electrical and optical results

- CH2: 5000 Hz, 200 µs period, 19.8–20.0 µs high pulse at 10% generator duty.
- Physical OUT1: high energizes the R5D; low turns it off.
- Webcam: blue-region metric 0.4 off, 210.0 at 5 kHz strobe, 254.8 continuous
  on. This proves light output but is not a duty-cycle measurement.
- A 35 µs camera strobe command did not produce a reliable current pulse.
- Commands 40–175 µs produced 17.6–144.2 µs current pulses. For commands
  >=50 µs, `measured = 0.9969 * commanded - 31.75 µs`.
- Manual-strobe LED current began about 78–79 µs after CH2.

## Exposure timing

Frame timestamps cannot resolve a 20 µs exposure. The live output capability
mask showed physical OUT2 is GPIO-only; OUT1 is the strobe-capable output.
An automatic-strobe exposure sweep used the SDK guarantee that auto strobe is
active during exposure. Current falling edge minus configured exposure gave
an inferred exposure start of 47.0 µs after CH2 (45.4–48.9 µs observed).

The previous 20 µs exposure therefore occupied approximately 47–67 µs while
the 100 µs-command current pulse occupied approximately 79–146 µs: no
overlap. A 100 µs exposure overlaps current for about 67 µs and is the next
bench starting point. Direct OUT1 probing in auto-strobe mode is still needed
to replace this inference with a hardware exposure-active trace.

## Measurement lessons

- Treat valid CH2 plus raw CH1 waveform as authoritative; Rigol CH1 automatic
  measurements returned invalid values for real triangular current pulses.
- Read every SCPI setting back immediately. Tightly batched commands were
  silently dropped during one rejected sweep.
- Stop, query acquisition sample rate/timebase, then download the waveform.
  The correct order returned 8192 points at 5 MS/s; the wrong order returned
  a stale 600-point display trace.
- Rigol grounds are common and earth referenced. Use the established circuit
  common and low-side shunt only.
- Restore generator frequency/duty to zero and camera output low after every
  bounded test.

## Durable runbook and data

See [`../../docs/howto/mindvision-xgc-r5d-led-strobe.md`](../../docs/howto/mindvision-xgc-r5d-led-strobe.md), including calibration CSVs and plots.
