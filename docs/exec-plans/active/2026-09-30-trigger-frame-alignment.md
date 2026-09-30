# Sort trigger ↔ frame alignment (multi-image mode, Euresys and MindVision rigs)

Status: active. Software layer (series identity, `/trigger_events`, line-event
hook, mock loopback) landed 2026-09-30; hardware loopback on Coaxlink and the
SSG3021X provenance service are next.

## Problem

When a cell is classified as a target, the PC drives a TTL edge (Coaxlink
`TTLIO12` / MindVision OUT2) into the **SIGLENT SSG3021X** RF generator's
TRIG IN, which — after its programmed trigger delay — emits the RF sort
burst. Nothing on that chain was recorded in the frame clock: the data file
knew which frame was classified (`isTargetGroup`) but not when the pulse was
driven, when the generator fired, or which exposures the burst overlapped. In
multi-image mode the extra series images carried no frame index or timestamp
at all, so a series could not be aligned to anything.

## Goal

Every sort pulse can be placed against the recorded frame sequence from the
HDF5 file alone: which frame was classified, when the PC drove the edge, when
the hardware saw it (where the rig can stamp inputs), and which series images
are which exposures — with an explicit flag when a series is not N consecutive
frames.

## Model

```
frame N exposed ──► classified target ──► TriggerService pulse ──► SSG TRIG IN ──► RF burst
  timestampNs          frameIndex            requestUs/wakeUs        (trigger delay)   PULSE OUT
  hostTimestampUs      grabUs                fireUs/pulseDoneUs                          │
                                                                                          ▼
                                                             loopback ──► camera/grabber input ──► lineEdgeTimestamp
```

Clock domains: `*Us` stamps are host monotonic µs (`Tools::getTimestamp`),
the same clock as `Frame::hostTimestampUs`. On Coaxlink/Windows the frame
timestamp is that clock too (µs since boot, QPC), so `fireUs` compares with
`timestampNs` directly. On MindVision the frame stamp is a 0.1 ms device tick
and only the host receipt stamp and a hardware loopback can bridge the gap.

## Acceptance criteria

- [x] `/valid_frames/series_meta` (+ `series_contiguous`) gives every series
      member its frame index, camera stamp and host stamp; a gapped series is
      flagged, not silently saved as consecutive.
- [x] `/trigger_events` holds one row per sort request with its real outcome
      and host stamps, always on, drained with the experiment flush.
- [x] `ICamera::setLineEventCallback` + `TriggerService::onLineEvent` pair a
      hardware-stamped loopback edge with its fired record; MockCamera
      emulates the loopback so the path runs headless.
- [x] Guards: `backend.trigger_event_log`, `recording.trigger_alignment_roundtrip`,
      `integration.e2e_series_alignment` (invariants I1–I5 in the test header).
- [ ] **Coaxlink loopback (Euresys rig):** SSG3021X PULSE OUT → spare grabber
      TTLIO input; `EGrabberCamera::setLineEventCallback` registers an
      EGrabber I/O-toolbox event callback (timestamped line events) and
      forwards rising edges as `LineEvent{timestamp = event stamp}` — the
      same µs-since-boot clock as `BUFFER_INFO_TIMESTAMP`. Windows-only code;
      verify the event/node names against the SDK sample tree before
      writing it (not compiled in the Linux CI lanes).
- [ ] **CIC cycle events (Euresys):** exposure-start stamps from the camera
      /illumination controller so a pulse is placed against exposure, not
      transport receipt.
- [ ] **`RfGeneratorService` (SSG3021X over USB/USBTMC, LAN later):**
      readback-first SCPI client — `*IDN?`, RF frequency/power/output state,
      pulse-mod mode (must be external trigger), pulse width, trigger delay,
      slope — snapshotted into the run's provenance at start; preflight gate
      refuses a sorting run when the generator is not armed; optional set of
      width/delay from the experiment config. Timing never goes over USB.
      Pattern: [[PulseGeneratorService]]-style verify-before-write; tests on
      a simulated instrument.
- [ ] **MindVision rig:** camera-tick ↔ host-clock fit with an error bound,
      stored in the file; loopback only via an LED in the field of view or a
      scope (no stamped inputs).
- [ ] **Hardware-aligned pulse (optional):** Coaxlink I/O toolbox one-shot
      armed by the PC and released on the next cycle trigger, so the edge
      lands on a frame boundary by construction.
- [ ] Review tab / Python reader consume `series_meta` and `trigger_events`.

## Decision log

- 2026-09-30: Records are always on (bounded ring, drained by the flush) rather
  than gated by `MIB_PIPELINE_TIMING`: the data file, not a diagnostics CSV,
  must say when a pulse fired. One clock read per stage on the trigger thread
  is within the existing latency budget (`integration.e2e_trigger_timing`).
- 2026-09-30: Loopback edges pair FIFO with fired pulses, not by time —
  the edge clock is not host-comparable on every backend, and the pulses are
  driven in order. Edges without a pulse are counted, never attached to the
  next session's pulse.
- 2026-09-30: `series_meta` row width comes from the on-disk extent on append
  (a batch whose first series is partial must not shrink earlier rows).
- 2026-09-30: The SSG3021X USB link is control/provenance only. Sub-ms
  timing comes from TTL and, where possible, hardware-stamped loopback.

## Progress

- [x] Software layer (this plan's first PR).
- [ ] Rig wiring: PULSE OUT → Coaxlink input; confirm TTLIO12 → TRIG IN.
- [ ] EGrabber line-event implementation + on-rig evidence
      (`docs/evidence/`), scope check of PC edge vs PULSE OUT.
- [ ] RfGeneratorService + provenance attributes.
