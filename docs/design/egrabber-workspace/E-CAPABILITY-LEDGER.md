# E capability ledger and integration contract proposals

Status: initial code/source inventory for #806, 2026-10-22. **0/9 complete
fold-in acceptance scenarios proved.** Presence of code or a historical merged
PR is not a test result from this execution.

Baseline: `7133749621defac01a80985fe6a3a5fc3f13eae1`, verified live workspace
branch head. Execution plan: [E fold-in](../../exec-plans/active/2026-10-22-egrabber-fold-in.md).

## Sources and evidence limits

- Original October 13 research: *MIB eGrabber design handoff*, *findings*, and
  evidence index for Studio 26.09.0.5 were recovered and read. Archive digest
  `a106333cb7ffb1c65f8d3d09298a94382f5734240d943b03b493a8fb5e445ccf`.
  Static recommendations: dynamic module/selector access, operating reserve,
  independent preview failures, recorder lifecycle and source recovery.
  Acquisition-start evidence `ev_a8ad692e818004429dd1034cdd5cda54abf42d6ec624a4b858319e7bcde702ba`;
  writable-query evidence `ev_8c8f653dbd77a7c5e6ecb58ba25e0547c54ca598dc35cb7c2cbc14e1bfb321cd`.
  Recorder decompilation covers only 77 bytes of a 2,276-byte symbol; it does not
  prove drain/retry behavior. No vendor code/assets copied into this repository.
- [Reference registry](reference.json) and [Penpot source map](PENPOT.md) retained.
  Read-only live page metadata on 2026-10-22: file
  `202e7953-c6e1-8109-8008-c8bf58f9647a`, page
  `a06b0eae-6a8a-8053-8008-c6fbecadf017`, **revn 124, vern 0**, 5,830 shapes.
  Revision-bound shape projection confirms root child count 37 and reads bounded
  text from refinement boards. Full-page native read returned an internal error;
  this is not proof that objects are absent. No fresh full visual/native-route
  audit is claimed. Historical native routing gaps stay open.
- The existing MIB authorities are the reuse solution: CameraControl, discovery,
  Capture, FrameStore, Recorder, Hdf5, Playback and shared Review. There is no
  reason to introduce another product, recorder, SDK owner or scientific engine.

## Capability → source → implementation → acceptance

All code paths below are relative to the repository root. Test names are
**located evidence candidates**, not newly executed results. PR associations
are historical source-map anchors unless explicitly verified by this execution.

| ID / source boards | Disposition and existing authority | Owned implementation issue / next gap | Acceptance evidence and limit |
|---|---|---|---|
| E01 · 06, 23, 29, 31; REA discovery/start | **extend** `src/backend/discovery/DeviceDiscoveryService.cpp`, `include/backend/discovery/DeviceDiscoveryTypes.h`, `desktop/src/discovery.ts`; backend has provider errors, generation and read-only discovery | #419/#272; #806 E-A1 owns response-identity guard. Camera list projection still loses provider failure detail; selected-source reconnect needs session-generation agreement | `tests/backend/device_discovery_{service,fault,stress,providers}_test.cpp`, `tests/integration/e2e_device_discovery_lifecycle_test.cpp`, `desktop/src/discovery.test.ts`. Missing: supported Windows camera loss/reconnect run and end-to-end busy/missing producer distinctions |
| E02 · 01, 05, 09–13, 33; REA writable/selector | **reuse + missing** ROI readback (`desktop/src/workspace/{alignControls,quickControlModel,useQuickControl}.ts`), script/config paths; `CameraControlService.h` exposes enumerate/script/reset, **not** a typed module/selector feature API | #716 A3 / #272; #806 E-B retains typed exposure/gain/FPS/trigger, categories/visibility and selector rollback as required. Bridge coordinator must assign ABI only after contract agreement | `desktop/src/workspace/{AlignView,quickControlModel}.test.*`, `tests/backend/camera_script_apply_test.cpp`. Missing: real feature access/constraints, forbidden writes, selector rollback and reconnect-race acceptance |
| E03 · 01, 05, 09, 13; image-centred views | **reuse + extend** `desktop/src/workspace/{AlignView.tsx,alignImageModel.ts}`; one delivered packet sink and bounded FrameInspector (#729 historical anchor) | #716 A3 / #806 E-B; multipart/channel and raw-versus-displayed provenance still require adapter qualification | `desktop/src/workspace/{AlignView,alignImageModel}.test.*`: coordinates, probe, histogram, profile and freeze fixtures. Missing: sensor ROI/part/channel provenance through live adapter; no scientific pixel mutation proof from hardware |
| E04 · 02, 07, 14–15, 32; REA independent preview/start | **reuse + extend** `CaptureService.cpp`, `EGrabberCamera.cpp`, `desktop/src/workspace/LiveView.tsx`; SDK start sequence already uses one `grabber_->start()`, EveryFrame/LatestFrame already exist | #328/#331/#333/#365/#368 and #806 E-C. Keep capture receipt, first frame, preview conversion and writer states distinct; do not reimplement acquisition | `tests/backend/capture_lifecycle_test.cpp`, `tests/integration/e2e_pipeline_stress_test.cpp`, `desktop/src/workspace/LiveView.test.tsx`. Missing: preview conversion fault while writer succeeds, actual supported-device disconnect/start observation and timestamp qualification |
| E05 · 03, 08, 34; REA operating reserve | **reuse + extend** `src/backend/playback/{FrameStore,PlaybackService}.cpp`, `desktop/src/{previewBuffer.tsx,ringPlayback.ts}`, `MonitoringCellStore.cpp` bounded copies | #806 E-C with existing ring work (#649); no duplicate buffer allocator. Reserve/budget semantics across host SDK and PZ ring are not interchangeable | `tests/backend/frame_store_{reserve,bounds,resize_under_load,concurrency}_test.cpp`, `desktop/src/{previewBuffer,ringPlayback}.test.*`, `tests/backend/monitoring_cell_store_test.cpp`. Missing: full stop/reallocate/no-auto-resume scenario, retained-range eviction/exact playback and native SDK reserve qualification |
| E06 · 04, 14–16, 35; REA recorder API | **reuse + extend; historical deferral reconciled** `src/backend/recording/{RecorderService,Hdf5Service,HdfExportService}.cpp`, existing run lifecycle and export jobs | #806 E-C; #807 owns orchestration, not a new writer. Chapters remain capability-qualified obligation; no chapter support inferred from HDF5 presence | `tests/recording/{recording_lifecycle,recording_accounting,hdf5_resilience,hdf_export_service,experiment_roundtrip}_test.cpp`. Missing: full range export/reopen + disconnect/disk-full/cancel/failed-close scenario at this head; explicit chapter capability |
| E07 · 06, 23–29, 31; preparation | **reuse + extend** `ExperimentCoordinator.cpp`, `desktop/src/workspace/{PrepareView.tsx,usePrepareReadiness.ts}`, hardware controls; readiness cache delivered (#719/#727 historical anchors) | #713 backend no-path/no-write test now passes (see native evidence below); #796–#799 externally delegated. #806 E-D owns remaining adapter-qualified LED/focus/storage/compatibility gaps, not the externally owned redesign | `tests/backend/{experiment_readiness,readiness_probe_cache}_test.cpp`, `desktop/src/workspace/usePrepareReadiness.test.tsx`. Missing: idle-only storage qualification, supported firmware/LED/focus evidence, no-actuation complete navigation with real backend |
| E08 · 11–12, 17–18, 27; engineering | **reuse + extend** `desktop/src/cameraScript.tsx`, existing config/update/help routes and backend script guard; provider support is qualified | #716/#272/#806 E-D; firmware/service tools must retain mode/idle/authorization checks. No normal navigation-triggered maintenance | `desktop/src/cameraScript.test.tsx`, `tests/backend/camera_script_apply_test.cpp`, `tests/hardware/hw_egrabber_script_test.cpp`. Missing: provider capability/version diagnostic report and supported firmware maintenance acceptance; hardware test not run |
| E09 · 04, 16, 19–22, 36; exact evidence | **reuse + extend** `MonitoringCellStore.cpp`, `desktop/src/workspace/useLiveCells.ts`, `desktop/src/review/inspect/inspectModel.ts`; one native reader/inspection owner | #806 supplies frame identity/delivery; #808/#717 own shared scientific inspector. F-contracts/1 logical semantics acknowledged; concrete integration ownership pending; do not fork Review | `tests/backend/monitoring_cell_store_test.cpp`, `tests/review/review_inspect_test.cpp`, `desktop/src/review/inspect/inspectModel.test.ts`. Missing: cross-track source/run/frame/object + processing/calibration identity and absent/expired stale-pixel clearing with real adapters |

Coverage bookkeeping: 00 is the complete-workspace map; 30 the evidence/delta map.
The rows assign every view 01–29 and 31–36, with intentional overlaps. This is
37-view **scope coverage**, not 37-view visual or implementation acceptance.

## Explicit contract proposals (logical v1, not ABI reservations)

### C-E1: SourceCapabilities — E producer; I/F consumers

Reuse the existing discovery `job_id`, `generation`, provider, stable identity,
identity strength and typed error codes. Discovery generation is a scan identity,
**not** automatically the connected camera session generation. Add capabilities
at the camera session owner with a confirmed source/session token. Each feature
key includes module, feature and selector-context identity. Descriptor has type,
implemented/readable/writable, access reason, visibility/category, numeric
min/max/increment or enum choices, current value and descriptor revision.

A write carries source/session + descriptor revision and request identity;
backend validates access and returns requested/pending/readback/error. Stale
session writes fail before touching hardware. Selector exploration is one bounded
backend transaction with restoration on success, error and cancellation. Do not
model it as successive UI writes. Unsupported provider is explicit. No second
SDK connection may bypass capture ownership. Owner agreement: #716 + bridge
coordinator; consumer acknowledgement #807/#808.

### C-E2: acquisition outcome — I request; existing coordinator/E result

Adapt existing start request/outcome and expected-run Stop (ABI 40 precedent),
not a new queue or recorder. Preserve experiment/sample/method, request identity,
actual run ID and start generation as decimal strings. Accepted means command
accepted, not first-frame receipt. Preserve unknown/refused outcomes so a retry
cannot duplicate an opened run. Frame delivery, preview and writer states have
separate errors. Closed/saved requires authoritative successful finalization and
accounting, never a Stop-button acknowledgement. I owns guided sequencing.

### C-E3: EventSnapshot / EvidenceResolver — E/native owner to shared inspector

Reuse the existing live-cell bounded-copy store and independent review reader.
Event reference: source fingerprint/session, run/epoch, set/row, frame/object/track
IDs (lossless decimal strings), committed watermark/gaps and processing,
calibration and mechanics revision. Mark absent fields absent, not zero.
Resolver returns owned bounded binary handles or an explicit not-recorded,
expired, unsupported, source-closed or error result. Returned identity must equal
the selection token. Clear previous pixels before requesting new evidence and
reject late replies. Never give F/I the recorder's mutable HDF5 reader.
The exact compatible DTO extension is pending #808/#717 agreement.

### C-E4: export/retention capabilities

Existing owners expose capacity/reserve/eviction, applied versus draft allocation,
raw/scientific output kind, supported chapters and range semantics. Applying a
capacity change follows the existing safe stop/reallocate path without restart.
Existing ExportJob owns progress/cancel/error/artifact/terminal recovery; E does
not implement a second worker. Original source data is immutable. Any missing
wire fields go through the bridge coordinator, not an independently reserved ABI.

## Reconciliation and blockers (not removed scope)

1. **Shared contract gate:** #716/bridge coordinator + #807/#808 must acknowledge
   C-E1–C-E4 before shared file/ABI changes. This audit proposes them but cannot
   assert peer agreement. Existing classic-view ownership remains unchanged.
2. **E02 backend gap:** no generic feature/selector API in the inspected camera
   utility service. Owner #806 with #716 session/bridge coordination. Resume:
   agreed single-owner extension, assigned ABI, provider fixtures and real adapter.
3. **Hardware qualification:** #272/#331/#333/#365/#368 remain open; supported
   Windows EGrabber hardware and existing actuation authorization are required.
   Owner: hardware operator + #806. No new authorization inferred from this audit;
   no device opened, reset, scripted, illuminated or moved by this execution.
4. **E07 external changes:** #796–#798 are delegated elsewhere; owner #799.
   Consume their reviewed changes when available; do not replace their sessions.
5. **E09 peer integration:** #808/#717 owns one inspector. Agreement on exact
   identity/unavailability fields and real-adapter tests is still required.
6. **Design evidence:** native routing and a full current visual pass remain
   unverified. Owner #806 design verification; metadata pin is not a click test.
7. **Product reconciliation:** later proposals restore required scientific scope
   under #808; October 15 “reference-only” language does not delete it. Existing
   Quick look statistics stay in the current backend authority until a reviewed
   shared scientific contract reconciles them. E does not modify scientific code.

## Reproducible partial demonstration

At this branch head, from `desktop/`: `npm ci`, then
`npx vitest run src/discovery.test.ts src/workspace/AlignView.test.tsx src/workspace/alignImageModel.test.ts src/workspace/LiveView.test.tsx`.
These exercise the actual React/polling modules with injected versioned-contract
fixtures. They are **not** a real-backend or hardware module demonstration.
The complete discover → failed-open recovery → configure → inspect → acquire →
freeze → retain → record → finalize → reopen → range-export scenario remains
unaccepted until the dependencies above are resolved and the real adapter runs.

## Verified history at the pinned base

These merge commits are ancestors of the inspected workspace HEAD (checked with
`git log HEAD`), not assumptions based only on old issue prose:

- `243f80fc` / PR #719: readiness probe cache.
- `f857b169` / PR #727: Prepare shell.
- `f1614019` / PR #729: Align image tools/ROI.
- `f471d3c4` / PR #728: start identity.
- `004829eb` / PR #744: start outcome / Stop generation.
- `1108951d` / PR #749: bounded live-cell images.

## Execution evidence — E-A1 milestone only

- Production scope: existing discovery poller; no new public contract or UI.
- Five wrong-job/generation/u64 regression cases failed against unchanged
  production code, then passed with the identity guard. Self-review found a
  coalesced waiter cancelling another caller's scan on timeout; its regression
  failed before the additional fix and passed afterwards. Fifteen discovery
  tests now pass. An owning caller's existing timeout/cancel test still passes.
- Full desktop suite: **1,442 passed, 1 skipped**, 121 passing test files and one
  skipped file. Fixtures exercise production modules, not native camera hardware.
- `npm run build`: TypeScript and Vite pass. Vite reports the existing large-chunk
  advisory; no native Tauri binary build is claimed.
- `python3 scripts/check_docs.py`: pass. `git diff --check`: pass.
- `node --test docs/design/egrabber-workspace/prototype.test.mjs`: **14 passed**;
  offline reference simulation only, not Penpot-native routing or real backend.
- Focused image/live/discovery suite before the coalescing addition: **81 passed**;
  the final full suite includes these tests and the additional regression.
- Self-review checked lossless comparisons, invalid-snapshot handling, all three
  terminal states, coalescing ownership, no cancellation on mismatched identity,
  and absence of backend/ABI changes. No independent reviewer is claimed.
- Remaining limitation: a backend restart that reuses *both* job ID and generation
  cannot be detected by this contract. A connected-source/session token belongs
  to C-E1; this slice does not claim complete reconnect safety.

Coordination notices were posted to #806, #716, #807 and #808 before shared
implementation. No acknowledgement is inferred from posting a proposal. Shared
contract changes, full live demonstration and E01–E09 acceptance remain blocked
as listed above. No merge, deployment or hardware action was performed.

## Native qualification follow-up (2026-10-22)

Continuing independently of pending bridge work, the workspace was configured
with `cmake --preset linux-backend-only -DMIB_ENABLE_MINDVISION=OFF` (system
libraries; local non-system package prefixes excluded). Built targets
`mib_backend_tests` and `hdf_export_service_test` with 12 compile workers.

- **24 native CTest cases pass:** discovery service/fault/providers/stress/facade
  and device lifecycle; capture lifecycle; FrameStore bounds/reserve/concurrency/
  resize; MonitoringCellStore; experiment readiness; new empty-path probe-cache
  proof; HDF5 resilience/export; experiment/run-identity/provenance/multi-image
  round trips; recording lifecycle/accounting; pipeline timing/stress.
- The #713 gap now has a real-backend test: 20 empty-path polls with a cold cache
  and another 20 with a warm destination cache, no cwd/previous-destination
  changes (including transient create/delete), output Unavailable and no claimed
  round-trip gate. Capture bootstrap is disabled. This is storage access/format
  qualification, not sustained storage throughput.
- Built `yofo-studio-server` with `MIB_BRIDGE_NO_CMAKE=1 cargo build --release`
  against those native libraries. The real server's `cargo test --release
  --test ws -- --test-threads=1` passes **11 tests**: mock capture/binary frame
  transport, client-loss stop/save (including stalled simulated pump), controller
  exclusion, run-specific Stop/start request handling, exact live cells,
  calibration/readiness and quick-look transport. No production service was
  started or replaced; tests bind temporary loopback listeners.

The native tests use synthetic images/simulated providers and pseudo-terminal
serial fixtures. They are stronger than UI DTO fixtures but **not physical
EGrabber/Windows SDK or timing proof**. Full E01–E09 acceptance remains open.
E07's missing no-path test is now implemented; its other requirements remain.

E acknowledged F-contracts/1 and I's ABI-40/snapshot-v3 semantics; I acknowledged
E's immutable evidence semantics. Unbiased committed analytic snapshots remain
separate from display-ring samples and HDF5 acquisition exclusion is preserved.
Concrete shared ABI/adapter ownership still needs coordination, but it does not
block isolated implementation or further qualification.


## Typed-feature isolated slice — PR #812

[PR #812](https://github.com/gavinlouuu-kpt/mib-studio-qt/pull/812) targets the
same workspace baseline independently of this ledger/discovery PR #809. It adds
`camera/features/FeatureControl` and a borrowed `EGrabberFeaturePort`, not a new
camera owner. Exact source/session/capability tokens fence stale calls; scalar
writes refresh access/bounds and distinguish requested values, readback and
unknown write outcomes. One SDK-inferred selector slot uses the SDK transaction;
rollback/range checking disabled means refusal without changing SDK options.

Evidence: two standalone CTests pass (fake core port and fake SDK adapter), real
26.09.0.5 SDK explicit template instantiation compiles, and the core passes UBSan.
The new CI workflow runs SDK-free core acceptance. No claim of physical rollback,
Windows linking, session-owner serialization or installed application controls.
Category/tree enumeration, multi-selector browsing, engineering commands and
shared ABI/capture-owner integration remain required E02 work. #716 has the
concrete allocation/ownership request; bridge45/Review7 remain reserved elsewhere.

Next real-adapter acceptance requests narrow shared test ownership in
`crates/mib-bridge-server/tests/ws.rs` for stopped-buffer resize, invalidated
identity, range export and no auto-resume. No shared implementation edit proceeds
on the assumption that merely posting a request grants ownership. This is a
specific shared-file dependency, not a general halt to independent E work.
