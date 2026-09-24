# Illuminated Live View on the rig PC — 2026-09-15

Software-side hardware evidence for PR #414 / issue #413, gathered with the new
`hardware.illuminated_live` test (`tests/hardware/hw_illuminated_live_test.cpp`)
driving the complete `AppBackend` path: MindVision selection at boot, saved
profile with `live_view`, automatic generator discovery, coordinated start,
frames, coordinated stop, then a fresh read-only Modbus readback of the
generator. **No oscilloscope measurement is included**; frame counts and the
latest frame's mean grey level are supplementary evidence only.

## Environment

- Host: Windows 11 rig PC, MSVC 2022 Build Tools 17.14 (toolset 14.44), CMake
  4.4.3, Ninja 1.13.2, Conan 2.32 (`ci` profile + cpuinfo pin), `windows-ninja`
  preset with `MIB_ENABLE_HARDWARE_SDKS=OFF`, `MIB_USE_SENTRY=OFF`.
- Camera: MindVision MV-XG51GM, GigE 169.254.34.249, S/N 056082722114, ROI
  512×96, installed platform 2.1.10.195 (pinned version).
- Generator: Zhongsheng pulse module, Modbus address 1, on COM6 (CH344 port A);
  channel 1 wired to the camera trigger input. A second, never-configured
  module answers on COM4 (all registers 0).
- Illumination: camera OUT1 (SDK output 0) → R5D LED driver.
- Command per run: `MIB_TEST_ILLUMINATED_LIVE=1 MIB_CAMERA_MODE=mindvision
  MIB_MINDVISION_CONFIG=<profile> MIB_TEST_RUN_SECONDS=4|5 MIB_TEST_RESTARTS=n
  build-ninja\Release\mib_backend_tests.exe hw_illuminated_live_test`.
- Installed MIB Studio Qt was closed (it otherwise holds COM6).

## Results

Every run: discovery resolved COM6 and excluded COM4 ("channel 1 reads 0 Hz"),
the camera armed with all readbacks matching (exposure 100.00 µs unless noted),
the lifecycle reached Running, Stop ended Idle with no shutdown failure, the
generator was released, and the independent readback showed channel 1 duty 0.
Transport-lost and discarded frame counters were 0 throughout.

| Log | Profile | Frames/s (host count) | Latest frame mean grey |
|---|---|---|---|
| [run2](logs/hw-illuminated-live-run2.log) | shipped preset: high-level trigger, polarity 1, 5000 Hz / 10 % — 3 cycles | 4490 / 4465 / 4564 | not sampled |
| [baseline-5000fps](logs/hw-illuminated-live-baseline-5000fps.log) | same, 2 cycles | 4592 / 4570 | 255 / 255 |
| [adjusted-2500fps](logs/hw-illuminated-live-adjusted-2500fps.log) | high-level, 2500 Hz / 5 % | 4572 / 4497 | 255 |
| [level-1000fps](logs/hw-illuminated-live-level-1000fps.log) | high-level, 1000 Hz / 2 % | 4525 | 255 |
| [edge-1000fps](logs/hw-illuminated-live-edge-1000fps.log) | rising edge, polarity 1, 1000 Hz / 2 % | 997.7 | 255 |
| [edge-1000-strobe1](logs/hw-illuminated-live-edge-1000-strobe1.log) | rising edge, polarity 1, strobe 1 µs | 998.0 | 255 |
| [edge-1000-exp20](logs/hw-illuminated-live-edge-1000-exp20.log) | rising edge, polarity 1, exposure 20 µs / strobe 20 µs | 997.8 | 255 |
| [edge-1000-delay900](logs/hw-illuminated-live-edge-1000-delay900.log) | rising edge, polarity 1, strobe 20 µs delayed 900 µs | 997.3 | 255 |
| [edge-1000-pol0-delay0](logs/hw-illuminated-live-edge-1000-pol0-delay0.log) | rising edge, polarity 0, strobe 20 µs delay 0 | 997.5 | 255 |
| [edge-1000-pol0-delay900](logs/hw-illuminated-live-edge-1000-pol0-delay900.log) | rising edge, polarity 0, strobe 20 µs delayed 900 µs | 997.4 | **52.2** |
| [default-1000fps](logs/hw-illuminated-live-default-1000fps.log) | **new bundled preset**: rising edge, polarity 0, 1000 Hz / 2 %, exposure 100 µs, strobe 100 µs — 3 cycles | 997.1 / 996.3 / 1000.2 | 255 |

## Conclusions

1. **High-level trigger (signal type 2) does not deliver one frame per pulse.**
   The frame rate stayed near 4500 frames/s at 5000, 2500 and 1000 Hz: the
   camera free-runs near its readout limit at this ROI. Rising-edge trigger
   (signal type 0) tracks the generator exactly (997–1000 frames/s at 1000 Hz).
2. **Strobe polarity 1 leaves OUT1 idling high on this rig.** With polarity 1
   the image stayed saturated regardless of strobe width (1 µs) or delay
   (900 µs, outside the exposure). With polarity 0 the image went dark
   (52/255) when the strobe could not overlap the exposure and saturated when
   it did — brightness follows the strobe only with polarity 0.
3. The bundled preset, the preset button and the parser defaults were changed
   accordingly: signal type 0, polarity 0, 1000 Hz / 2 % (20 µs pulse), the
   operating point chosen by the user for now.
4. **Open, needs the oscilloscope:** the LED-current waveform (CH1) against the
   trigger (CH2) for the new preset — one pulse of about the strobe width per
   trigger, nothing between triggers, nothing after Stop. Also the exposure
   edges. Frames saturate at 255 even with a 20 µs strobe, so the R5D current
   or the optical path needs attenuation before optical use; that is a rig
   adjustment, not a software gate.
5. The camera's real limit with edge trigger is about 4500 frames/s at 512×96;
   requesting more in the FPS box will not be honoured by the sensor.
