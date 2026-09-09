# Glossary

> Domain and code terms you'll run into. Short definitions; follow
> `[[WikiLinks]]` for detail.

## Microscopy / measurement

- **Deformability** — shape deviation from a perfect circle. Computed in
  [[../services/ProcessingService]] from contour moments. Low = round,
  high = elongated.
- **Area** — contour pixel count converted to μm² using
  `pixelToMicronFactor_` (default 0.4886 μm/px).
- **Ring ratio** — ratio of outer/inner contour radius. Used for focus
  feedback — drives [[../services/AutofocusService]] via
  `RingRatioCallback`.
- **Brightness quantiles** — Q1/Q2/Q3/Q4 percentiles of pixel intensity
  within the contour mask. Part of `FilterResult`.
- **Young's modulus (E-modulus)** — stiffness. Computed by LUT lookup from
  `(area, deformability)`; LUT loaded from
  `resources/isoelastic_curve/*.txt` by `AppBackend::initialize`.
- **Target group** — a second gate within valid frames; frames matching
  target-group criteria pulse the camera trigger
  ([[../services/TriggerService]]).
- **ROI** — rectangular region of interest; applied pre-analysis by
  [[../services/ProcessingService]] and by display in [[../frontend/PreviewPage]].

## Portability

- **Portable processing contract** — the frozen combination of the
  gold-standard metrics JSON shape, `ProcessingConfig` JSON shape, and
  Young's-modulus LUT text format that a non-Qt consumer (e.g. Biowork's
  `services/mib-processing`) must match to get identical results. Defined in
  `docs/gold_standard_metrics.md` ("Portable Processing Contract" section)
  and `docs/gold_standard_metrics.schema.json`.
- **`contract_version`** — single integer naming one frozen version of the
  portable processing contract above. Bump it whenever the metrics schema,
  config schema, or LUT format changes incompatibly. Currently declared
  independently in six places (`test_contract_version_consistency.py` guards
  against drift until/unless that's folded into one source of truth).
- **Processing core registry** — versioned engine metadata published by
  `publish-processing-core.py`: a complete short-cache active pointer at
  `{channel}/processing-core/latest.json`, immutable manifests under
  `versions/<version>.json`, and an enumerable `index.json`. Schema v2 pins
  the canonical core/contract version, hash-qualified Python wheels, optional
  signed native plugins, profile catalog, and emodulus LUT as one reproducible
  set. The generated PEP 503 page supports baked `mib-processing==<version>`
  dependencies. See `docs/portable-processing-sync.md`.
- **Processing core active version** — the full manifest named by both
  `latest.json` and `index.json.active_version`. Publishing or rolling back a
  channel changes these mutable pointers; it never rewrites immutable version
  history, and clients with an explicit version pin ignore the pointer.
- **Native processing core** — a versioned, platform-signed shared library
  selected by [[../frontend/ProcessingCoreDialog]] and loaded through the
  POD-only C engine ABI. Windows uses Authenticode; Linux uses detached
  Ed25519 signatures pinned to a compiled signer SPKI SHA-256 (A13/#245
  keeps the live signed publication gate open). ABI v1 owns mask generation
  and empty-frame classification; host metrics/tracking/orchestration remain
  outside it.
- **Processing conformance reference** —
  `scripts/gold_standard_dataset.json`, a deterministic full-parity output from
  the installed wheel. `scripts/run_processing_conformance.py` fails on metric,
  mask, series-image, target-group, tracking, or record-accounting drift.

## Protocols & SDKs

- **GenICam** — standard camera control API.
- **SFNC** — GenICam Standard Features Naming Convention. Features like
  `DeviceReset` are SFNC-defined.
- **PFNC** — GenICam Pixel Format Naming Convention. Codes used in
  `Frame::pixelFormat`.
- **Euresys EGrabber** — SDK for CoaXPress framegrabber + cameras. See
  `egrabber-sample-programs/` and `docs/integration/egrabber.md`.
- **StreamModule** — EGrabber statistics module
  (`StatisticsFrameRate`, `StatisticsDataRate`). Must refresh before
  stopping capture.
- **Acquisition trigger** — the signal that starts a camera exposure.
  `trigger_mode` in the MindVision JSON config: 0 free-run, 1 software
  (`softTrigger()` / `CameraSoftTrigger`), 2 external (TTL edge on the camera
  trigger input). See [[../camera/MindVisionCamera]]. Distinct from the sort
  trigger below — opposite signal direction.
- **Sort trigger (sort-output pulse)** — TTL pulse the camera's GPIO emits
  toward the sorter when a target group is detected, driven by
  [[../services/TriggerService]] via `setTriggerOutput`. NOT an acquisition
  trigger.
- **Strobe** — camera output synchronized to exposure, used to fire
  illumination. MindVision modes: 0 auto-sync with exposure, 1 manual
  (delay + pulse width), 2 always high, 3 always low.
- **Pulse generator** — Zhongsheng RS485 module producing the external
  acquisition-trigger pulse train (400 Hz–40 kHz, duty-gated on/off); driven
  by [[../services/PulseGeneratorService]]. Identified by (bus, serial
  settings, Modbus slave address); the output channel is a setting below
  that identity.
- **RS485 bus session** — one shared `QSerialPort` owner per physical
  USB/RS485 adapter ([[../services/SerialBus]]); RS485 is multi-drop, so
  several Modbus devices at different slave addresses share one adapter and
  all requests on it are serialized with strict response correlation.
- **Modbus RTU** — serial protocol used by
  [[../services/SyringePumpService]] (Sample + Sheath pumps) and
  [[../services/PulseGeneratorService]], framed by `ModbusRtu.h` over
  [[../services/SerialBus]].
- **Coremor XMT** — serial protocol for the piezo nanopositioner used by
  [[../services/AutofocusService]]. DLL under `include/Coremor/`.
- **ONNX Runtime** — ML runtime for [[../services/YoloService]].

## Code idioms

- **PIMPL** — "pointer to implementation" pattern. Hides third-party
  headers (HDF5, ONNX). Used by [[../services/Hdf5Service]] and
  [[../services/RecorderService]].
- **Absolute write index** — monotonic 64-bit counter in
  [[../data-model/FrameStore]]. Wraps only if the ring does, but indices
  themselves never decrease.
- **FrameStore filter mode** — frame filter that returns `true` to SKIP
  (used by recording mode to drop empty frames).
- **Frame delivery mode** — user-facing SDK-queue policy in
  [[../camera/ICamera]]: **Every Frame** (ordered, never intentionally
  skipped; backlog grows under overload) vs **Latest Frame** (stale
  completed SDK buffers are drained before the copy; every deliberate
  discard is counted). Applied at the earliest controllable SDK queue,
  not in [[../data-model/FrameStore]]. Intentional discards, transport
  loss/underrun, and downstream processing drops are separate counters
  by contract.
