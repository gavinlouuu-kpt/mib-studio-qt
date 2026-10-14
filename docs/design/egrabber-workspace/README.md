# eGrabber-inspired workspace: implementation brief

Status: design baseline; production integration pending.

This is the branch-local source of truth for implementing the reviewed MIB
wireframes. Keep the existing application working while migrating its interface.
The goal is eGrabber-style direct, image-centred control plus MIB cytometry,
not a vendor clone and not a second backend.

## Reference hierarchy

1. Real backend capabilities, safety constraints and authoritative readback.
2. The accepted full workspace: Prepare -> Align -> Experiment -> Review.
3. The reviewed v2 [interaction reference](prototype.html) for failure, readback,
   retention and persistence behavior. It contains seven refinement views only.
4. Static eGrabber 26.09.0.5 analysis and the public 23.04 tutorial inform controls;
   neither establishes untested MIB hardware behavior.

The earlier wireframes contain illustrative values. Prefer the reviewed v2 state
semantics when they correct an earlier screen. Missing backend data is Unknown or
Unsupported, never a simulated green check. Changes to the approved workflow must
be documented in the [execution plan](../../exec-plans/active/2026-10-14-egrabber-workspace.md).

## Complete workspace scope

| Workspace / reference views | Required outcome |
|---|---|
| Prepare (06, 23-29) | Camera/source discovery; required vs optional devices; LED driver; motor and piezo focus; storage capacity and throughput; software compatibility and optional updates; pass/blocked recovery states |
| Align (01, 05, 09-13) | Image-centred live view; typed camera/GenApi controls; pixel probe; fit/zoom/pan; histogram; line profile; quick exposure/gain/ROI/trigger controls; scripts and advanced engineering tools |
| Experiment (02, 07, 14-15) | Independent acquisition, display and recording controls; live cell preview with Area-Deformability plot; polygon and optional E-range selection; multi-source and run/writer health |
| Retained-frame playback (03, 08) | Exact frame/range navigation; history duration and capacity; raw-image identity; safe reallocation; explicit eviction; TIFF/AVI export |
| Saved-run review (04, 16) | Open saved sources; exact record/image/mask/measurement association; playback speed and chapter/time navigation; scientific HDF5 distinct from vendor containers |
| Scatter inspector (19-22, 36) | Click a point or row to inspect the exact event, frame and object; raw/contour/mask modes; live pinned selection; per-record pending/saved/missing image states |
| Maintenance (17-18, 27) | Firmware and driver compatibility, power-cycle status, preferences/help, diagnostics; no automatic firmware or software install during a run |
| Engineering refinements (30-36) | Source recovery; independent stream/preview/writer status; selector-scoped access; buffer ownership; finalization; provenance |

Keep reference views 00-29 accessible in the planned information architecture.
The seven views in prototype.html are not the final application navigation.

## Prepare means instrument readiness

**LED driver:** identify the physical controller/channel, supported interface and
capabilities; distinguish requested settings from available readbacks, including
current, temperature, output state and interlocks. Configuration must leave output
off/inhibited. Camera exposure synchronization, polarity, delay and pulse width
are capability-dependent; never invent a sensor/interlock that is not fitted. Any
light test requires explicit operator authorization through existing safety gates.

**Motor and piezo:** distinguish coarse motor steps from a verified home reference,
and commanded piezo position from measured position. Show homing state, limits,
travel envelope, calibration, faults and settling only when supported. Device
discovery must not home or move a stage. An open-loop piezo must not be labelled
closed-loop. Autofocus and motion use the existing backend services.

**Storage:** selected output path, mounted volume identity, permissions, free space,
policy reserve, estimated run size and sustained write qualification belong to one
preflight snapshot. Use actual payload/parts, camera count, retention, duration and
measured overhead. Unknown compression savings are not guaranteed capacity. Disk
benchmarks are explicit, bounded and idle-only; use a unique temporary test file,
never truncate an existing file. Keep raw recording and scientific HDF5 budgets
distinct and never offer conflicting writers as concurrently available.

**Software:** show installed app/SDK/driver/firmware/core versions and compatibility
separately from update availability. Optional updates are advisory. A required
compatibility/security failure blocks only the relevant operation under documented
policy. Offline or stale manifests are explicit. Installation requires consent, an
idle instrument, integrity checks and a supported rollback/recovery plan; recheck
readiness after updates. Browser-only clients must respect platform capability
limits rather than exposing desktop-only installers.

## Interaction contracts

- Start requested, command acknowledged and first frame observed are different
  states. Acquisition, analysis, preview conversion and writer health remain distinct.
  Freeze display is not Stop acquisition, Stop recording or buffer reallocation.
- GenApi identity includes session/generation, source, module, feature and selector
  context. Refresh implementation/read/write access, bounds and increments. A
  requested value is not current until readback confirms it; write-only parameters
  explicitly have no readback. Disable forbidden actions and reject invalid values.
  Selector exploration is serialized/transactional and restores the original selector.
- Pool allocation, retained history, in-flight buffers, inspection holds and available
  operating slots have different meanings. A history slider edits a proposal. Apply
  only via a backend-owned safe stop/reallocate/announce/queue sequence. Do not
  auto-resume. A zero free-slot instant is not proof of a lost frame.
- Pin an owned image copy or stable persisted reference, not an unbounded SDK
  buffer lease. Copies need memory limits and eviction policy; no available image
  means metadata-only inspection with a reason.
- Stop recording -> stopping -> drain -> finalizing -> confirmed closed, or error.
  Preserve counters and pending work across failures/retry. A frontend button may
  not invent persistence confirmation. Export copying at 100% is not verified complete;
  cancellation and failure preserve the input source.
- A point resolves using run/session, device/stream/part/frame, object, timestamps,
  processing/calibration/LUT revisions and evidence ownership. Clear prior pixels on
  selection; reject stale responses. Preserve u64 identities without JavaScript precision
  loss. Per-record image/mask availability is independent of measurement availability.
- Keep Area on X and Deformability on Y. E is obtained from the applicable validated
  LUT, shown as applicable curves and an optional interval combined with the polygon
  using AND. Out-of-domain E remains unavailable. The reference's E curves and values
  are illustrations, not a mechanics model. Inspect never edits the gate.
- Multiple objects in one frame are not one record; multiple frames are not a tracked
  cell unless tracking actually exists. Original stored analysis and reprocessing with
  current settings are separately labelled and never silently overwrite each other.

## Reuse current code; do not duplicate authority

At the branch base the existing implementation includes these entry points:

| Concern | Existing starting point |
|---|---|
| React shell and workflow | desktop/src/App.tsx, workflow.ts, preflight.ts, quality.ts, contextBar.ts |
| Hardware permissions | desktop/src/commissioning.ts, transport/control-role checks, backend authorization |
| IPC and remote clients | desktop/src/bridge.ts, desktop/src/transport/, crates/mib-app-commands/, crates/mib-bridge/ |
| Backend service composition | src/backend/app/AppBackend.cpp and the BackendFacade boundary |
| Devices and focus | DeviceDiscoveryService, CameraControlService, AutofocusService, StageService, TriggerService |
| Capture / persistence / review | FrameStore, CaptureService, ProcessingService, RecorderService, Hdf5Service, PlaybackService |
| Updates | desktop/src-tauri/src/updater.rs and platform capability checks |

Audit the exact DTOs and tests before each slice; names in this table are navigation,
not a claim every required UI capability is bridged. Extend the versioned bridge
additively when needed. Keep Qt/React, Windows/Linux, host/FPGA and remote-viewer
behavior truthful; do not fake host processing on a PL-backed platform.

## Publication and acceptance

The public branch contains original explanatory prose and a vendor-free simulation.
Private Penpot host links, operation IDs/recovery tooling, credentials, real cell
data, font binaries and proprietary/decompiled vendor code are deliberately omitted.
The sanitized prototype preserves the reviewed v2 behavior; its changes are recorded
in [reference.json](reference.json).

The initial commit does not install Penpot links, replace production UI, or actuate
hardware. Pass browser interaction checks for the reference and then build separate
React/backend integration tests. Never present reference tests as hardware parity.
