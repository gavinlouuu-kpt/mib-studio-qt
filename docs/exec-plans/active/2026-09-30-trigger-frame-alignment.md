# Sort trigger ↔ frame alignment (multi-image mode, Euresys and MindVision rigs)

Status: active. Software layer (series identity, `/trigger_events`, line-event
hook, mock loopback) and the SSG3021X SCPI service (readback, readiness gate,
provenance) landed 2026-09-30; hardware loopback on Coaxlink is next.

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
- [x] **`RfGeneratorService` (SSG3021X over USBTMC or LAN):** readback-first
      SCPI client (`*IDN?` gate, `:OUTPut?`, `:PULM:STATe/SOURce/MODE?`,
      `:PULM:TRIGger:MODE?`, `:PULM:DELay?`, `:PULM:WIDTh?`, …), `rf.generator`
      readiness gate, `rf_generator_*` provenance attributes,
      `applySortWindow` with readback verification. NI-VISA is loaded at
      runtime on Windows (not compiled in Linux CI — verify on the rig).
      Not yet: setting the window from the experiment profile; Review tab
      display; frontend config UI for the `rf_generator` block.
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
- 2026-09-30: SCPI set taken from the current SSG3000X Series Programming
  Guide (the 2018 guide predates `PULM:TRIGger:MODE` / `PULM:DELay`); NI-VISA
  is loaded at runtime rather than linked so a PC without VISA still starts.
- 2026-09-30: the readiness gate never writes to the instrument — an
  unarmed generator is reported with the menu/SCPI remedy, not fixed
  silently.
- 2026-10-01: SCPI over LAN (port 5025), not USB — no NI-VISA dependency on
  the rig PC; the VISA transport stays as an option. Loopback input is
  TTLIO11 (same Internal I/O 1 header as the TTLIO12 sort output, TTL
  levels, valid `LineInputToolSource`). EGrabber delivery must use
  `processEvent<IoToolboxData>` on a dedicated thread because the camera
  holds `EGrabber<CallbackOnDemand>` and pops frames.

## Progress

- [x] Software layer (this plan's first PR).
- [x] Rig wiring decided (2026-10-01, rear-panel photo): the sort coax is in
      TRIG IN/OUT and PULSE IN/OUT is free → PULSE OUT → Coaxlink Internal
      I/O TTLIO11: the rig breakout is an HL-DB26T-mini on External I/O (HD26) → terminal 25, GND terminal 24 (photos in `docs/evidence/2026-10-01-trigger-loopback-wiring/`).
      LAN chosen for the SCPI link. Hand-over for the rig agent:
      [`2026-10-01-trigger-frame-alignment-rig-handoff.md`](2026-10-01-trigger-frame-alignment-rig-handoff.md).
- [ ] Rig (parked until a scope is attached): both breakout TTL lines are
      used (TTLIO12 sort, TTLIO11 LED); choose IIN11 or Internal I/O 2
      TTLIO21 for the loopback, then confirm on the scope that PULSE OUT
      pulses once per trigger in Ext-Trig mode.
- [ ] Rig (now, hand-off §6a): Windows build, SSG over LAN, readiness gate
      green on the real instrument, one recorded run with the new datasets.
- [ ] EGrabber line-event implementation + on-rig evidence
      (`docs/evidence/`), scope check of PC edge vs PULSE OUT.
- [x] RfGeneratorService + provenance attributes (2026-09-30).
- [ ] On-rig: USB (NI-VISA) link check, `*IDN?`, readiness gate green with
      the real instrument; decide whether the profile sets the sort window.
