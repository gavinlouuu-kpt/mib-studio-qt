# ADR 0008 — YOFO Studio: the PZ7035 runs all per-frame science in the PL; the PS runs the headless backend, a preview-only camera path and a remote React UI

- Status: proposed (for review)
- Date: 2026-09-30
- Issues: mib-studio-qt #441 (E1 #445, E2 #446, E3 #447, E4 #448), pz7035-imx426 #3, #7
- Plan: [2026-09-30-yofo-studio-pz7035](../exec-plans/active/2026-09-30-yofo-studio-pz7035.md)
- Amends: ADR 0001 (React + Tauri), the E0 ADR "Standalone Zynq-7035 target and ownership"
  (numbered 0006 on `feat/441-embedded-e0-target-contract`, colliding with develop's 0006), and
  pz7035-imx426 ADR 0001 (its amendment is pz7035-imx426 ADR 0002)

## Context

The PZ7035-FH (XC7Z035, dual Cortex-A9 at 666 MHz, 1 GiB DDR) is to be the standalone
instrument. On 2026-09-30 Linux booted on its PS for the first time with the camera image
running in the PL (pz7035-linux `docs/pz7035-live-pl-boot.md`). Measured on that board:

| Measurement | Value |
|---|---|
| `memcpy` of one 512x96 Mono8 frame | 158 us (311 MB/s) |
| CPU readout of one frame from the PL grabber over GP0 | 1.6 ms; sustained 500 fps |
| TCP send from the PS | 303 Mbit/s, one core saturated |
| Sensor raw rate at 5000 fps | 245.76 MB/s |

One frame copy costs most of the 200 us frame period. The A9 cannot see every frame, so
nothing that must run per frame can run on the PS. The E0 ADR already placed sensor ingress
and "accepted high-rate science" in the PL, but left target selection, gating and tracking
provisionally on the PS (U12), assumed a local 1024x600 touchscreen, and forbade Qt6 and
Tauri on the target. The carrier has no LCD or HDMI connector; the touchscreen would need a
custom adapter on the 40-pin header, whose pin 5 now drives the LED strobe.

The MIB per-frame path today (`ProcessingService::realtimeInlineLoop`) is: ROI, blur,
background subtract, threshold, empty check, morphology, contours, per-object metrics,
gates, target decision, one trigger pulse per frame, and buffering of raw frame + mask for
HDF5. The KU5P repository (IMX426-YOFO dc5eb74) holds simulation-verified RTL for the mask,
CCL, aggregate and metrics stages and a 64-byte M4S1 record; the pz7035 ABI v1.1 defines
FRAME/RESULT/EVENT/PREVIEW/IMAGE records and a bridge co-simulated byte-exact against its
model. None of the classical science has run on a board.

## Decision

1. **Product identity.** The PZ7035 instrument version of the application is **YOFO
   Studio**. User-visible identity changes (product name, window title, bundle identifier,
   package and installer names, update feed, HDF5 `software` attribute). Internal names stay:
   the repository, CMake targets, the `mib_processing_*` C ABI, `MIB_*` macros and
   environment variables, and the `MIBF` packet magic. "MIB" remains the internal code name.

2. **Frontend.** React + Tauri (ADR 0001) is the only UI line for YOFO Studio; the Qt frontend
   is not ported to ARM. On the instrument the React app is served by the PS and used
   remotely in a browser (or the Tauri desktop shell) over the network. No local display is
   planned for this version; the E0 touchscreen (E4 #448, LVGL) is deferred, not cancelled.
   The E0 rule "no Qt6, no Tauri on the ARM target" stands: neither runs on the PS.

3. **Placement of per-frame work.** Everything that runs per sensor frame runs in the PL:
   mask, connected components, per-object metrics, E-modulus lookup, validity gates, target
   decision, timed sort output, and recording selection. The PS never sees every frame and
   never synthesises results. This resolves E0 unknown U12 in favour of the PL. Tracking is
   not per-frame today (batch/review only) and stays on the PS.

4. **Two data paths from the PL, both consumed by the headless backend on the PS.**
   - **Records at full rate.** FRAME, RESULT and EVENT records (pz7035 ABI) through the
     kernel driver (`/dev/pz_mib0`, DMA ring). At 5000 fps with a few objects per frame this
     is about 2 MB/s, which the A9 consumes. They feed accounting, monitoring rows, trigger
     status, autofocus and the HDF5 metadata tables live.
   - **Pixels never at full rate to the CPU.** Live view is a preview stream (tens of fps)
     through a GenTL producer on the PS, consumed by Aravis and the existing `AravisCamera`
     backend in `LatestFrame` mode. Recorded images (raw + mask) go PL -> frame store -> SSD
     at full rate and are joined to the metadata after the run.

5. **Camera control.** The sensor, strobe and lane-order features are GenICam features served
   by the same producer from `pz7035_sfnc.xml`; `CameraControlService` sets them through
   Aravis. The processing profile, gates, background and LUT are not camera features: they go
   through the driver's `SET_CONFIG`/commit path from the PL execution-provider adapter (E3).

6. **Science parity is defined by the bundled kernel.** The PL classical profile must
   reproduce `subtract-ring` Contract 1 (blur, saturating subtract, threshold, cross
   close/open, inner-contour objects, hull metrics, ring ratio, quantiles, E-modulus, gates)
   bit-for-bit where integer, and within stated tolerances where the host uses doubles. The
   conformance vectors are the host kernel's outputs. Contract 2 features (absdiff,
   preprocessing chain, Laplacian variance) are a later profile.

7. **Background.** The PS computes the background (auto and calibrated, from preview frames;
   both are idle-time operations with no deadline) and loads it into a PL background bank;
   the PL swaps banks at SOF and reports `BACKGROUND_EPOCH`. The PL does not average frames.

8. **Recording.** The PL store filter implements MIB's selection (trigger-anchored valid
   frames, every Nth invalid frame, N following frames in multi-image mode) so the SSD sees
   only what MIB would have kept. HDF5 assembly (metadata live from records, images from the
   store drain) runs on the PS after the run and is not on any frame deadline.

9. **Host surface removed on the instrument.** EGrabber/MindVision, the eGrabber camera
   script, `PulseGeneratorService` (RS485 acquisition-trigger train; the PL times XVS and
   the strobe itself), `TriggerService`'s pulse thread (the PL times the sort output),
   `YoloService`, and Sentry upload.

## Consequences

- E3 (#447) is the largest software work: a PL execution provider that turns the record
  stream into `FilterResult` rows, accounting and trigger status, and compiles
  `ProcessingConfig` into profile parameters, gate words, LUT and background uploads. The
  pz7035 ABI needs a parameter page larger than seven words (ABI 1.2, board repository).
- The largest hardware work is on the board: a run-based streaming CCL and a pipelined
  metrics engine that close 512x96 at 5000 fps on 7-series (the KU5P blocks fit in area but
  not in cycles), plus gates, target decision, store filter and the E-modulus LUT. Tracked in
  pz7035-imx426 `docs/processing/MIB_PARITY_PORT.md`.
- New shared piece: a network transport for the bridge (a Rust server on the PS speaking the
  existing bridge contract over WebSocket, serving the React app) and a transport switch in
  the frontend. The bridge contract itself does not change.
- `FrameStore` is sized per target (a preview ring of tens of frames, not 5000).
- The E0 manifest (`deploy/embedded/pz7035-target.json`) must be updated on merge: display
  class "none (remote React client)", `ownership` rows for gating/target/trigger to PL,
  reuse row for the Aravis camera, the Tauri/React entries in `dependency_allowlist`
  (host client only), and the touch-UI topology invariant replaced by "the remote client
  talks only to the PS runtime". Also renumber the E0 ADR (0006 collides with develop).
- Evidence rule unchanged: nothing here is proven on hardware until the exec plan's gates
  record physical evidence. Camera, Linux and inference images remain separate evidence.
- Local, PC-free operation (E0 policy) is weakened for this version: the instrument needs a
  browser somewhere on the network. The controlled Stop & Save on client loss must therefore
  hold for a lost WebSocket client, not only a lost touch process.
