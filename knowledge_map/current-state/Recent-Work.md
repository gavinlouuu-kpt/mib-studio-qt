# Recent Work

> Snapshot of recently merged features and fixes, as of 2025-11 / 2025-12.
> Refresh from `git log --oneline -20` when outdated.

## Features shipped

- **Fix: nanopositioner probe rejected a resting stage** (2026-09-08) — the
  CoreMorrow controller at 0 V reports about -1 mV; `PROBE_VOLTAGE_MIN` was
  0.0, so `AutofocusService::probeComPort` failed about one run in four
  (`hardware.nanopositioner` flaked 1/4 on the bench) and the app's boot
  path fell back to "saved nanopositioner COM6 did not validate; scanning all
  ports". Floor is now -0.05 V. See [[../services/AutofocusService]].

- **Fix: app locked out of the EGrabber camera at boot on beta fa6e6ba
  ("No camera found" while the Connect tab lists it)** (2026-09-08) —
  Bench validation of `v1.0.7-beta.fa6e6ba` (Coaxlink Quad CXP-12 /
  EoSens 2.0MCX12): the standalone hardware tests streamed, but the app
  reported "No camera found" and every later Connect / capture start /
  camera-script apply failed with `GenTL error -1004, GCInitLib: Requested
  resource is already in use`, 4 of 4 launches. Mechanism, isolated with
  direct probes: only one Euresys `EGenTL` may exist per process, and the
  MindVision SDK's CoaXPress plugin (`CXPCamera_X64.Interface`) loads
  `coaxlink.cti` and calls GCInitLib itself during `CameraEnumerateDevice()`;
  with the vendor SDK installed it wins the race at boot and every later
  EGrabber open fails for the life of the process. Stable v1.0.7 was
  unaffected because MindVision was compiled out (`42cd7a54` enabled it).
  Fix: a process-wide shared GenTL handle (`GenTLHolder.h/.cpp`,
  `backend::camera::egrabber::sharedGenTL()`) used by
  [[../camera/EGrabberCamera]] and every discovery/apply path in
  [[../services/CameraControlService]]; `discoverMindVisionCameras()`
  reserves it before calling the MindVision SDK, so the plugin's attempt
  fails harmlessly and both camera families work in any order (an earlier
  interim fix on this branch skipped MindVision enumeration whenever a
  framegrabber was present; superseded because a bench can hold either
  camera). New hardware test `hardware.discovery_reentry` replays the boot
  sequence with modes for thread, MindVision step, and MindVision-first
  order; it failed before the fix and passes after. Files:
  `GenTLHolder.h/.cpp`, `EGrabberCamera.h/.cpp`, `CameraControlService.cpp`,
  `src/backend/CMakeLists.txt`, `tests/hardware/hw_discovery_reentry_test.cpp`.

- **Fix: `-terminate` crash artifacts never produced on Windows for
  exceptions escaping a worker thread** (2026-09-07) — the beta
  pipeline's Windows CI (CTest in `build-windows.yml`) was failing
  `backend.crash_reporter_terminate` on every `develop` push since issue
  #347's fix (011f565), blocking the auto-beta release. Root cause:
  [[../services/CrashReporter]]'s `SetUnhandledExceptionFilter(sehHandler)`
  silently replaces the CRT's own top-level filter, which is what
  normally recognizes the MSVC C++ exception SEH code (`0xE06D7363`) and
  calls `std::terminate()`. Without that translation, an exception
  escaping every C++ frame never reached `terminateHandler` —
  `sehHandler` intercepted it first, wrote generic `-seh.*` files, and
  let Windows tear the process down directly. Two `sehHandler`-based
  attempts changed nothing, which showed the real mechanism: MSVC's
  `std::thread` shim is `noexcept` (terminate is called in the search
  phase, the top-level filter never runs), MSVC's `set_terminate` is
  **per-thread** so the worker thread only ever had the CRT default
  `abort()` (→ `-sigabrt.*` via the SIGABRT fallback), and MSVC's
  `current_exception()` is null for uncaught exceptions. Fix: a
  first-chance vectored exception handler that, on every C++ throw on any
  thread, lazily installs `terminateHandler` for that thread and captures
  `what()` from the exception record's ThrowInfo into a `thread_local`
  record that `terminateHandler` writes into `-terminate.txt`;
  `sehHandler` forwards a C++ exception that does reach the top-level
  filter to `std::terminate()`. The test now lists the crash dir on
  failure. Verified locally (Linux): full `linux-backend-only` CTest
  suite green. Windows-only code path; only verifiable end-to-end via the
  `build-windows.yml` CI lane. With that green, the same lane exposed
  `camera.delivery_mode_contract` as timer-resolution-flaky on Windows:
  `QueueBackedTestCamera` slept in <=1 ms slices, but a default ~15.6 ms
  Windows tick made its "2000 fps" producer run at ~64 fps -- the same
  rate as the quantized "slow" 5 ms consumer -- so the queue never
  exceeded depth 1 and LatestFrame never discarded (pass/fail depended on
  whether something on the runner had raised the timer resolution). The
  producer is now deadline-based with catch-up bursts (bounded to one
  queue's worth per wake), keeping the nominal rate honest on any timer.
  That in turn exposed a latent race in `camera.delivery_mode_overload`'s
  lag metric (producer head read *after* the grab, outside the lock, while
  underruns consume sequence numbers): the head is now sampled under the
  grab lock (`newestCompletedSequenceAtLastGrab()`), making "lag at grab" a
  true logical distance.
  Files: `CrashReporter.cpp`, `crash_reporter_terminate_test.cpp`,
  `tests/support/queue_camera.h`.

- **Shared RS485 bus + Linux serial discovery for the pulse generator**
  (2026-08-31, issue #323 follow-up) — the pulse-generator stack is now
  usable on Linux and correct on multi-drop RS485. New
  [[../services/SerialBus]] layer: `SerialBusManager` hands out one
  `ModbusBusSession` (exclusive `QSerialPort` owner) per
  (system port name, baud/data/parity/stop); all transactions are serialized
  with strict request/response correlation (`modbus::classifyResponse` /
  `expectedFrameLength` in `ModbusRtu.h`) — wrong-address frames are
  discarded not misattributed, CRC errors and trailing bytes surface as
  possible duplicate-address collisions, exceptions carry their code.
  [[../services/PulseGeneratorService]] became an addressed bus client:
  `connect(QString portName, SerialSettings, addr)` (no more synthesized
  `COMn`), typed `LinkError` status, and a read-only cancelable
  `scanBus(1..16)` that classifies pulse generators vs generic Modbus
  devices vs corrupt responders and never writes.
  [[../services/SyringePumpService]] transport was ported onto the same
  manager (COM-number API unchanged), so a pump and a generator can share an
  adapter. [[../frontend/ConfigTabs]] gained a `QSerialPortInfo` port
  dropdown + Refresh, data/parity/stop combos, Scan (worker thread,
  cancelable), and QSettings persistence including USB serial/VID/PID for
  device-node re-resolution. Post-review hardening (same day): each bus
  session runs its `QSerialPort` on a dedicated I/O thread (no cross-thread
  races with the GUI event loop); open failures classified from the typed
  `QSerialPort::error()` enum, not translated strings; session teardown
  closes the port and erases the registry entry atomically under the
  registry lock; inter-frame delay measured from last bus activity and a
  shorter collision listen (less GUI-thread latency for pump polling);
  `identityLooksLikeGenerator` plausibility gate on connect and scan so a
  random 12-register Modbus device is never adopted/written into; scan
  reports port-acquire failures distinctly from a silent bus; pump
  `connect()` no longer self-deadlocks on reconnect and gained a `QString`
  port-name overload. Test: `backend.serial_bus_pty` (POSIX pty bus
  simulator: two generators + generic device on one bus, concurrency,
  correlation failure states, scan write-safety, impostor refusal, pump
  reconnect regression). Files: `SerialBus.{h,cpp}`,
  `ModbusRtu.h`, `PulseGeneratorService.{h,cpp}`,
  `SyringePumpService.{h,cpp}`, `AppBackend.{h,cpp}`, `ConfigTabs.{h,cpp}`.

- **Sentry default-ON in all presets** (2026-08-31) — the three Linux
  presets (`linux-release`, `linux-system-release`, `linux-backend-only`)
  no longer force `MIB_USE_SENTRY=OFF`, so every preset now compiles
  sentry-native (matching the option's default and `windows-default`).
  `backend-ci.yml` gained `libcurl4-openssl-dev` (the Linux curl
  transport hard-requires it) and now exercises the CrashReporter sentry
  paths on every CI run; the python-wheel workflow's Linux plugin build
  passes `-DMIB_USE_SENTRY=OFF` explicitly (wheels don't ship crash
  reporting, matching its Windows job). Offline configure still degrades
  gracefully to local-only crash reporting.

- **Crash-reporting gap fixes** (2026-08-31, issue #347) — three
  informativeness gaps left after #346. (1) The `std::terminate` handler
  now writes its minidump unconditionally (Crashpad never sees
  terminate/abort, and the SIGABRT fallback `_Exit`s behind the
  `handlingCrash` guard, so nothing else captured this path) and records
  the unhandled exception's `what()` in a `.txt`; background-thread
  uncaught exceptions now reach Sentry with a real stack on the next
  launch. (2) Verified against pinned sentry-native 0.7.20: envelopes
  waiting in the transport queue at shutdown are persisted and re-sent,
  but a *failed* send drops the envelope — so stale `.dmp.queued` files
  (older than `Config::queuedRetryAfterDays`, default 7) get exactly one
  re-submission, renamed to the terminal `.dmp.queued2`. (3) Retention
  now also bounds never-submitted pending `.dmp` files, so local-only
  (no-DSN) installs cannot grow the crash dir without limit. Tests:
  `backend.crash_reporter_pending_upload` (stale-retry + pending
  retention cases) and new child-process test
  `backend.crash_reporter_terminate`.

- **Shutdown crash prevention** (2026-08-26) — Sentry crash analysis
  revealed SIGSEGV crashes occurring after all services stopped, during the
  C++ destructor chain. Root cause: cross-service callbacks (camera-ready,
  target-group, background-capture) could fire into already-destroyed
  `unique_ptr` services due to reverse-declaration-order member destruction.
  Fixes: (1) `AppBackend::shutdown()` now clears all cross-service callbacks
  before stopping any service, (2) `MainWindow::closeEvent()` now stops the
  capture service before window destruction begins, (3) `OverviewTab` and
  `MainWindow` destructors now stop their timers before `delete ui` to
  prevent use-after-free on `backend_` during widget teardown. Files: `AppBackend.cpp`, `MainWindow.cpp`, `OverviewTab.cpp`.

- **Crash-reporting minidump attachment fix** (2026-08-26, issue #345) —
  pending `.dmp` files are now submitted via `sentry_capture_minidump`
  (attaches the actual minidump binary) instead of `sentry_capture_event`
  (message-only). Custom SEH/signal handlers are no longer installed when
  Sentry/Crashpad is active, avoiding handler ownership conflicts; the
  `on_crash` callback writes the JSON sidecar instead. State snapshot
  extras are cleaned between dumps to prevent leakage. File lifecycle
  changed from `.uploaded` to `.queued`; legacy `.dmp.uploaded` files are
  recovered on startup. Bounded retention removes oldest `.queued` dumps
  beyond `maxRetainedDumps`. CI now runs `sentry-cli debug-files check`
  after symbol upload. Test: `backend.crash_reporter_pending_upload`.
  Post-review hardening (same day): pending upload is gated on
  `isSentryActive()` so never-sent dumps are not marked `.queued` and
  destroyed by retention; a SIGABRT fallback handler stays installed when
  Sentry is active (Crashpad cannot see CRT aborts); the `on_crash` hook
  guards reentrancy, snapshots once, and mutates the event instead of the
  scope; retention also bounds orphan `.json` sidecars and only logs
  removals that succeeded; directory scans collect paths before renaming
  (Windows `FindNextFile` can skip entries otherwise); the test suite was
  rewritten from `assert()` (vacuous under NDEBUG — CI builds Release) to
  an explicit failure counter.

- **Processing-core wheels for ARM64** (2026-08-07, issue #341) —
  advanced `mib-processing` to 0.2.1 and expanded the `manylinux_2_28` wheel
  release matrix from x86_64-only to
  CPython 3.10–3.13 on x86_64 and aarch64. Every architecture
  gets an in-container install/import smoke test (QEMU-backed for ARM64), while
  x86_64 retains the full pytest/conformance and Biowork-base gates. Release
  validation now requires exactly 8 Python/architecture wheel pairs. Linux
  i686 is intentionally excluded because the NumPy dependency ecosystem has
  dropped it. Task record:
  [[../task/2026-08-07-issue-341-processing-core-wheel-architectures]].

- **Windows beta test portability follow-up** (2026-08-03, issue #338) — the
  first default-on MindVision beta runner successfully provisioned the pinned
  R2 SDK and configured/compiled the complete Windows app, then exposed two
  test-only portability assumptions. The release gate now checks the Unix
  provisioner's executable bit only on POSIX filesystems, and the simulated
  camera overload workload pauses its consumer above the coarse Windows timer
  quantum while retaining queue-depth, sequence-distance, and cross-mode-ratio
  assertions.

- **MindVision enabled by default on every desktop OS** (2026-08-03, issue #338)
  — R2 contains pinned Windows x64, Linux x86/x64/arm/arm64, and macOS
  x86_64/arm64 SDKs. CMake and maintained presets now enable the real provider
  on all three OSes; platform provisioners verify checksums/APIs and Linux CI
  lanes provision before configure. Windows release paths also fail if
  `MVCAMSDK_X64.dll` is missing from the payload. Processing-only builds remain
  SDK-free. Regression guard:
  `scripts.mindvision_release_gate`; task record:
  [[../task/2026-08-03-issue-338-mindvision-release-builds]].

- **Explicit camera delivery modes** (2026-08-01, epic #328, issues
  #329–#334) — user-selectable `Every Frame` (ordered, complete) vs
  `Latest Frame` (freshest, intentionally discards stale SDK buffers)
  applied at the camera SDK queue itself. New contract in
  [[../camera/ICamera]] (`FrameDeliveryMode`, capabilities,
  `AcquisitionQueueStats` with distinct intentional/transport/underrun
  counters); [[../services/CaptureService]] rejects unsupported modes,
  reports the backend-confirmed mode, and polls queue/frame-age telemetry.
  [[../camera/EGrabberCamera]] gets corrected start sequencing (single
  `grabber->start()`, no camera-first 50 ms window) and a ScopedBuffer
  stale-frame drain (`BufferPartCount` forced to 1 in Latest Frame);
  [[../camera/MindVisionCamera]] gets newest-priority retrieval with an
  exact-counting bounded-drain fallback. Per-profile persistence via
  `camera.frame_delivery_mode` (AppConfigWatcher → the first-ever
  `CaptureService::setConfig` call site), status-bar badge, ConnectTab
  combo, and a blocking Latest-Frame acknowledgement before experiments.
  Tests: `camera.delivery_mode_overload`, `camera.delivery_mode_contract`;
  hardware runbook `docs/howto/camera-latency-mode-validation.md`; task
  note [[../task/2026-08-01-frame-delivery-mode]].
- **MindVision external-trigger acquisition + strobe sync + pulse-generator
  control** (2026-07-31, epic #323) — camera can now run software- (mode 1,
  `softTrigger()` end-to-end from a new MindVision tab in
  [[../frontend/ConfigTabs]]) or externally-triggered (mode 2) acquisition
  with strobe synced to exposure. New JSON keys `ext_trig_signal_type`,
  `ext_trig_jitter_us`, `acq_trigger_delay_us`, `trigger_count`; `strobe_mode`
  widened to [0,3]. Both config-apply paths unified through
  `MindVisionApply.cpp` (closed the 19-vs-7 field drift). Health check no
  longer steals triggered frames; `grabFrame` buffer copy moved under the
  state lock (stop() race); mono8 ISP output forced unconditionally (color
  sensor overrun). New [[../services/PulseGeneratorService]] controls the
  Zhongsheng pulse module (external trigger source) over Modbus RTU from the
  same tab. Task note: [[../task/2026-07-31-mindvision-external-trigger]].
  Tests: `backend.pulse_generator_frame`, extended `backend.mindvision_config`,
  extended facade boundary test. Hardware verification on the Windows rig
  still pending (#327).
- **Latency & target-identification-loss metrics** (2026-07-21, issue #292) —
  quantifies pipeline latency and lost sort targets. New counted losses:
  [[../services/TriggerService]] no-camera / set-failed pulse drops
  (previously silent `continue`s) and target-group objects beyond the first
  in a frame that never get a pulse (`selectTargetGroupTriggerOwner` serves
  only the first). New always-on identification funnel + invalid-reason
  histogram on [[../services/ProcessingService]] (`getIdentificationCounters`,
  reasons from the shared `science::classifyInvalidReasons`), a
  [[../diagnostics/PipelineTimingRecorder]] `summarize()` percentile view plus
  an always-on live acquisition→pulse latency gauge, status-bar surfacing,
  [[../diagnostics/CrashStateMirror]] loss fields, and a funnel/loss section in
  `analyze_pipeline_timing.py`. Test: `processing.identification_metrics`.
- **Trigger-path hardening** (2026-07-18, issue #227) — the
  [[../services/TriggerService]] thread elevates itself to
  `THREAD_PRIORITY_TIME_CRITICAL` on Windows (best-effort `SCHED_FIFO`
  elsewhere) so background load — e.g. the HDF5 writer draining an
  experiment flush — cannot preempt a pending LED/sort pulse (measured
  request→wake tail at default priority: p50 ~53 µs but max 30 ms of pure
  OS scheduling in a 20 s / 500 fps mock run). And
  `EGrabberCamera::setTriggerOutput` now caches the `LineSelector`
  selection per camera session (`triggerLineApplied_`, reset on start /
  reconfigure / write failure) so each pulse edge is a single `LineSource`
  register write instead of two — halving per-pulse PCIe transactions.
  Guard: `integration.e2e_trigger_timing`; before/after via the mock
  timing harness (`docs/howto/pipeline-latency-diagnosis.md`).

- **Release sync-PR step made truly best-effort** (2026-07-20) — the
  v1.0.7 stable dispatch pushed the tag, then died in the "Open
  fallback-version sync PR" step: `gh pr create` was denied ("GitHub
  Actions is not permitted to create or approve pull requests" — repo
  Actions setting), and although the `catch` printed its best-effort
  warning, the leftover non-zero `$LASTEXITCODE` failed the job before
  "Create GitHub Release" / R2 publish ran. The step now ends with
  `exit 0` so a caught sync failure can never block the release again.

- **Tag-first stable release flow** (2026-07-17) — `build-windows.yml`
  release mode no longer pushes to `main` (its branch protection rejected
  the workflow's own version-bump push with GH006, failing the first
  v1.0.7 attempt). The workflow now pushes only the annotated `vX.Y.Z`
  tag at the exact validated `main` SHA — version resolution is tag-based
  (`resolve_desktop_release_version.py` / `MIBVersion.cmake`), so the tag
  is authoritative — and opens an automated `chore/release-vX.Y.Z-sync`
  PR into `develop` for the `DEFAULT_VERSION` no-git fallback bump and
  refreshed manual screenshots (best-effort; never blocks the release).
  Lanes doc updated: `docs/howto/branching-and-releases.md`.

- **Realtime latency fixes: event-driven wake + trigger request queue**
  (2026-07-17, #282/#283) — `FrameStore::waitForFrame` replaces the realtime
  loops' fixed 2 ms sleep-poll with a condition-variable wake from
  `pushFrame` (Dekker-guarded waiter counter; one relaxed load per push
  while nobody waits), and `TriggerService` replaces its single-bool request
  flag with a bounded per-request queue (drop-oldest overflow counted via
  `getDroppedRequestCount()`). Measured with the mock harness (500 fps,
  `gavinlouuu/512x96stream`): grab→algo p50 1.03 → 0.083 ms, end-to-end
  grab→trigger-fire p50 1.44 → 0.50 ms, and exactly one pulse per target
  frame (was 5/2176 coalesced). Guards: `backend.frame_store_wait`,
  `integration.e2e_pipeline_timing` phases 3–4. Task record:
  [[../task/2026-07-17-realtime-latency-fixes]].

- **Pipeline latency instrumentation** (2026-07-17) — Added
  [[../diagnostics/PipelineTimingRecorder]], a lock-free per-frame latency
  recorder for diagnosing realtime-pipeline and trigger delay: host-monotonic
  stamps at acquisition (`Frame::hostTimestampUs`, stamped in
  CaptureService and carried through FrameStore), algorithm start/end,
  callback dispatch, and trigger request/wake/fire/pulse-done, with
  per-reason skip counters so frame accounting is conserved (no silent
  loss). `TargetGroupEvent`/`TargetGroupSignal` now carry source-frame
  identity; coalesced trigger requests are counted. Enable with
  `MIB_PIPELINE_TIMING=1`; CSVs dump on capture stop and are analysed by
  `scripts/analyze_pipeline_timing.py` (per-stage percentiles + end-to-end
  grab→fire). Guards: `backend.pipeline_timing_recorder`,
  `integration.e2e_pipeline_timing`. How-to:
  `docs/howto/pipeline-latency-diagnosis.md`. Task record:
  [[../task/2026-07-17-pipeline-latency-instrumentation]].

- **Release-lane gating for the develop/main pipeline** (2026-07-17) — The
  feature → `develop` (beta) → `main` (stable) lanes now gate what actually
  releases. The `develop` auto-beta in `build-windows.yml` is path-filtered
  to app-affecting changes, so docs/vault/`tools/`/`tests/`/wheel-lane-only
  merges (e.g. the Ultra96 exploration, PR #264 → stray
  `v1.0.6-beta.0e398db`) no longer publish a beta; edits to the workflow
  itself still release. `mib-processing-v*` GitHub releases are created with
  `--latest=false` so the repository "Latest" badge always points at the
  newest stable desktop release. Lanes documented in
  `docs/howto/branching-and-releases.md`.

- **Ultra96 direct-DDR FPGA pipeline** (2026-07-15) — Replaced the exact
  4-PPC/250-MHz image core's full-frame AXI-BRAM MMIO path with XRT/CMA buffer
  objects over the 128-bit PS HP0 DDR port. The board now matches all 171 MIB
  reference frames exactly (8,404,992 mask pixels, 920 ordered contours, 176
  records, 21 valid objects, 7 targets), asserts exact masks over the
  5,000-frame steady pass, and profiles contour extraction, hierarchy,
  metrics, and tracking separately. Serial performance improved from 260.5 to
  1,251 fps; two-buffer FPGA/ARM overlap reaches 1,344 fps. Task record:
  [[../task/2026-07-15-ultra96-direct-ddr]].

- **Kernel-owned scientific pipeline** (2026-07-14, A7/#242) — Moved contour
  extraction, per-object metrics/LUT/target gating, brightness quantiles, and
  batch track matching behind `IProcessingKernel`
  (`analyzeObjects`/`matchTrack`), with the single shared implementation in
  `src/backend/processing/ProcessingScience.cpp` and plain data contracts in
  `ProcessingTypes.h`. `ProcessingService` routes every batch/offline/
  realtime scientific decision through the selected kernel; threads,
  callbacks, track lifecycle state, and HDF5 stay host-owned. A
  pre-migration golden test (`processing.science_golden`) pins exact outputs
  and a spy kernel (`processing.science_seam`) proves the routing. The native
  plugin now compiles the science sources; the C ABI remains v1 (mask/empty),
  so ABI v2 object-record marshalling is the remaining A7 step.

- **Linux Ed25519 detached-signature trust adapter** (2026-07-14, A13/#245) —
  Implemented `verifyProcessingCoreEd25519` behind the injected trust seam:
  the immutable manifest transports a 44-byte DER SPKI and raw 64-byte
  Ed25519 signature over the artifact bytes, trusted only against the
  compiled `MIB_PROCESSING_CORE_ED25519_SPKI_SHA256` pin (OpenSSL libcrypto,
  Linux desktop builds only; wheel builds stay OpenSSL-optional and fail
  closed). The plugin now builds with hidden symbol visibility and a
  release-named `.so` + sidecar; catalog/settings/publisher/verify tooling
  validate the signature transport; CI gained a Linux build/audit/
  sign-rehearsal lane and the `processing.core_ed25519` CTest matrix with a
  cache→`dlopen` load. Spec:
  `docs/architecture/processing-core-linux-signing.md`. Live signed
  publication (real keypair, repository pin, Production job) remains open.

- **Cross-platform processing-core seams** (2026-07-14, A13/#245) — Removed
  DLL-only assumptions from registry publication/verification, made native
  signature scheme and requirement part of catalog/persisted identity, and
  gave Linux x86_64/aarch64 runtime fingerprints explicit platform identity.
  The C ABI, cache, and `dlopen` loader already exercise real `.so` fixtures;
  production Linux activation remains fail-closed until A13 supplies an
  audited detached-signature and release lane. Authenticode remains only the
  Windows trust adapter.

- **Processing-core release gate closure** (2026-07-14, A8/A10/A11/A12) —
  Added repaired `manylinux_2_28` wheels proven in a clean Python 3.12 base,
  independent native ABI fixtures, exact export/import auditing, secret-free
  Authenticode rejection tests, flat signed release assets, and a separate
  Production signing job. Release channels now follow stable/prerelease wheel
  identity; an authenticated Production workflow promotes or rolls back
  existing immutable versions and verifies public cross-links. Activation now
  rejects leases before reset, clears stale scientific state, and passes
  watchdog-protected A→B→A concurrency stress. Profile metadata carries an
  optional processing contract, downgrades require confirmation, and HDF
  regeneration warns on identity drift. Stable LUT revision `2026.07.14-1`
  is live at `updates.yofo.bio`. The remaining blockers are the A7 full-kernel
  boundary and A12's real production certificate/Windows hardware proof.

- **Portable CI guards for processing-core release tests** (2026-07-14) —
  Required C11 for the pure-C ABI layout test so MSVC evaluates its compile-time
  assertions, installed NumPy in backend and sanitizer CI for the HDF5
  conformance-input regression, and made the public Hugging Face Dataset Viewer
  integration skip only after exhausted transient HTTP/network retries.
  Payload-shape and scientific failures remain fatal.

- **Desktop release identity/artifact gates** (2026-07-14,
  [A12 #240](https://github.com/KPT1020/mib-studio-qt/issues/240)) — Hardened
  every desktop publisher against mismatched source refs and stale installer
  reuse. Manual tag dispatch validates and checks out the exact tag; manual
  stable CI defers commit/tag/push until tests, installer builds, exact-file
  checks, and artifact upload pass, then atomically pushes the tested main and
  tag. Local and Actions publishers clear old outputs and consume only the
  exact numeric-version Setup/Update files (including beta releases). One
  resolver now bumps from the highest reachable release line, and paired CMake
  overrides/readback keep the binary's numeric/full identity equal to its tag.
  Every entrypoint builds all tests and runs CTest before external publication;
  the local path requires a clean tree/main for stable, atomically pushes refs,
  and treats GitHub/R2 errors as fatal.
  Beta R2 keys include the full prerelease identity, and equal numeric SHA
  betas sort by publication time. Focused resolver/CMake/publisher regressions
  cover these invariants. Real PowerShell/Windows execution, production
  signing, GitHub/R2 publication, and hardware proof remain A12 gates.

- **Offline processing-core identity visibility** (2026-07-14,
  [A11 #243](https://github.com/KPT1020/mib-studio-qt/issues/243)) — Fixed the
  selector E2E defect where its active-core label remained blank until both
  registry documents loaded successfully. The dialog now renders the local
  active identity before starting the first request and keeps registry/network
  failures in the separate status label. A real offscreen Qt dialog regression
  proves both messages coexist when registry loading is refused.

- **User-guide website** (2026-07-13, issue #233) — `docs/manual/` is now
  published as a searchable site at
  <https://kpt1020.github.io/mib-studio-qt/>: new `mkdocs.yml` (Material,
  `docs_dir: docs/manual`), a visual-tour landing page
  (`docs/manual/index.md`) built from the generated screenshots, and
  `.github/workflows/docs-site.yml` deploying to GitHub Pages on pushes to
  `main` that touch `docs/manual/**` — including the screenshot-refresh
  commits from `build-windows.yml`, so the site tracks each release.
  Details: `knowledge_map/task/2026-07-13-user-guide-website.md`.

- **User manual + generated screenshots** (2026-07-13, issue #229) —
  New operator manual under `docs/manual/` (connect, acquire & record,
  review & post-process, troubleshooting) whose screenshots are generated
  by a new `screenshot_tour` executable
  (`src/frontend/tools/screenshot_tour_main.cpp`) that drives the real UI
  headless in mock-camera mode. `scripts/check_screenshots.py` (run by
  `docs-ci.yml`) fails when manual image references drift from the harness
  registry; `build-windows.yml` regenerates the PNGs each release and
  commits them back to `main`. Details:
  `knowledge_map/task/2026-07-13-user-manual-screenshots.md` and
  [[../frontend/Screenshot-Tour]].

- **Release-time processing-core signer trust gate** (2026-07-14,
  [A12 #240](https://github.com/KPT1020/mib-studio-qt/issues/240)) — All three
  maintained Windows desktop publishers now require the same GitHub repository
  `MIB_PROCESSING_CORE_SIGNER_SPKI_SHA256` before shipping stable/beta
  installers. CMake normalizes and validates exact 64-hex DER-SPKI pins while
  keeping the requirement disabled for ordinary development/fork builds; the
  manual workflow checks before its version commit/tag/push and the local
  publisher reads the destination repository variable before every
  non-skipped build and before bumping. The native-core tag job derives
  DER-SPKI SHA-256 from the DLL's actual
  Authenticode signer and rejects a repository-pin mismatch before upload.
  Eight focused tests cover CMake rejection/normalization and cross-path
  wiring. Real certificate secrets, repository pin provisioning, R2 publish,
  and Windows hardware proof remain A12 live gates.

- **Transactional processing-core settings persistence** (2026-07-14,
  [A10 #241](https://github.com/KPT1020/mib-studio-qt/issues/241)) — Fixed the
  desktop E2E defect where a native core became live before `QSettings::sync()`
  failed and was then marked unavailable. The persistence callback now runs
  under the core-selection lock after every operation guard and before the
  pointer swap, so failure preserves the prior usable core. The app now sets a
  stable `MIB Studio` settings identity, migrates all missing keys from Qt's
  former `Unknown Organization` namespace once without overwriting or deleting
  user data, and refuses startup if that migration cannot be synchronized.
  Filesystem-backed fault injection covers previous-selection restoration;
  activation tests cover commit ordering and old-kernel preservation. Live
  A→B→A and TSan stress remain A10 exit gates. Task record:
  `knowledge_map/task/2026-07-13-hot-swappable-processing-core-native.md`.

- **Native processing-core selector foundation** (2026-07-13,
  [A7 #242](https://github.com/KPT1020/mib-studio-qt/issues/242),
  [A8 #239](https://github.com/KPT1020/mib-studio-qt/issues/239),
  [A10 #241](https://github.com/KPT1020/mib-studio-qt/issues/241), and
  [A11 #243](https://github.com/KPT1020/mib-studio-qt/issues/243)) — Added a
  POD-only C engine ABI, independently built versioned native module, strict
  hash/Authenticode/identity/self-test loader, per-call context pool, resident
  module policy, and atomically prepared content-addressed cache. The new
  [[../frontend/ProcessingCoreDialog]] enumerates stable/beta history,
  derives channel-active state only from independently fetched `latest.json`
  (never the ahead-of-pointer index field), cross-checks immutable manifests,
  respects app ranges and administrator
  pins, restores verified selections at startup, and activates only between
  operations. Live/offline/playback mask paths plus recording/buffer empty
  filtering use the selected core; HDF5 stores exact operation provenance.
  ABI v1 deliberately leaves contour metrics, tracking, target decisions, and
  orchestration host-owned, so A7 remains open rather than claiming full
  pipeline replacement. Production signing/R2/Windows hardware proof remains
  the A12 live gate. Task record:
  `knowledge_map/task/2026-07-13-hot-swappable-processing-core-native.md`.

- **Versioned processing-core registry and automatic release publication**
  (2026-07-13, [A6 #238](https://github.com/KPT1020/mib-studio-qt/issues/238)
  + [A9 #237](https://github.com/KPT1020/mib-studio-qt/issues/237), under the
  [hot-swap epic #236](https://github.com/KPT1020/mib-studio-qt/issues/236)) —
  Extended the existing full `processing-core/latest.json` into an enumerable,
  rollback-safe registry: immutable `versions/<version>.json`, short-cache
  `index.json` with an independent `active_version`, and a channel-scoped PEP
  503 page with SHA-qualified links. Schema v2 adds canonical identity,
  filename/size metadata, and optional native plugin descriptors while
  retaining every schema-v1 consumer field. `--from-release` now derives and
  hashes the actual GitHub Release assets, validates wheel/tag/sidecar identity,
  refuses conflicting immutable keys or unreadable catalogs, and promotes
  `latest.json` last. Added atomic wheel/package version bump tooling with a
  post-commit tag guard and 27 unit tests across manifest/index/native/PEP 503,
  verifier shape, dry-run release derivation, bump/tag, and consistency
  behavior. The existing
  conformance workflow now covers CPython 3.10–3.13, gates a Windows native
  artifact and Authenticode signing, attaches one release set, and publishes
  R2 automatically. Real certificate/R2 execution remains the explicit A12
  live gate; it is not claimed from sandbox tests.

- **Installed-wheel full-parity conformance harness** (2026-07-13, issue #225,
  the final planned anti-drift stage of the [Biowork portability epic
  #220](https://github.com/KPT1020/mib-studio-qt/issues/220)) —
  `scripts/run_processing_conformance.py` runs the installed
  `mib-processing` wheel over a deterministic nested-contour/multi-image
  sequence and compares it with the committed
  `scripts/gold_standard_dataset.json`. `compare_metrics.py` now uses
  `(index, frame_type, object_id)` identities (so multi-object records cannot
  be silently collapsed), rejects extra candidate records, compares
  `youngs_modulus`, contract metadata, target-group/tracking fields, and exact
  SHA-256 evidence for every mask and ordered series image. The wheel's
  previously exposed `include_series_images=True` option is now functional:
  `ProcessingService::processBatch` attaches the trigger + available following
  frames for retained valid tracks, and binding dicts expose target/tracking
  metadata. `.github/workflows/python-wheel.yml` runs the harness against each
  wheel it builds and uploads the candidate for failure diagnosis. Local wheel
  verification also added conditional compatibility for OpenCV 5's new
  `opencv_geometry` split and upstream/Homebrew HDF5 2.x's un-namespaced CMake
  targets. The unavailable PANC1 input was replaced, with user approval, by a
  revision- and SHA-pinned eight-frame detection window from the private
  `gavinlouuu/z_adjustment-data` 50V in-focus HDF5 recording. The runner reads
  bounded HDF5 windows directly; only its metrics/output-hash reference is
  committed, while the 1.52 GB input remains ignored runtime data.

- **Processing-core sync manifest (`publish-processing-core.py`)** (2026-07-13,
  issue #224, part of the [Biowork portability epic
  #220](https://github.com/KPT1020/mib-studio-qt/issues/220)) — New
  `{channel}/processing-core/latest.json` manifest pinning the
  `mib-processing` wheel version (#223) + `contract_version` together and
  cross-linking the existing profile-catalog and emodulus-LUT manifests, so
  a non-Qt consumer (Biowork's `services/mib-processing`) resolves config +
  LUT + engine as one set from a single GET. `publish-processing-core.py`
  (uploads via the existing `scripts/s3_upload.py`, supports `--dry-run`)
  and `verify-processing-core-manifest.py` (stdlib-only reachability +
  shape check, mirrors `verify-emodulus-lut-manifest.py`) both live at repo
  root alongside their siblings. New doc
  `docs/portable-processing-sync.md` documents all three manifest schemas
  together; `docs/howto/auto-update-r2.md` gets the operational
  publish/verify runbook. Added `test_contract_version_consistency.py` as a
  drift guard: `contract_version`/`CONTRACT_VERSION` is currently declared
  independently in six places (surfaced while adding this manifest's own
  `DEFAULT_CONTRACT_VERSION` as a seventh candidate for drift) with no single
  source of truth; the test fails CI if any declaration is bumped without
  the others. Verified against live infrastructure: the manifest, wheel
  sha256 computation, and reachability checks (including a real 404 vs.
  real 200 on `updates.yofo.bio`) were exercised directly, not just unit
  tested; 6/6 new Python tests pass.

- **Python bindings for `mib_processing` (`bindings/python/`)** (2026-07-13,
  issue #223, part of the [Biowork portability epic
  #220](https://github.com/KPT1020/mib-studio-qt/issues/220)) — pybind11
  module (`_mib_processing`) wrapping `process_batch`, `compute_processed_frame`,
  `EModulusLut`, and the `BatchMaskSources` load/save functions over the
  Qt-free `mib_processing` core, all speaking the gold-standard metrics dict
  shape (`docs/gold_standard_metrics.md`). Packaged via scikit-build-core
  (`bindings/python/pyproject.toml`), which drives the *same* root
  `CMakeLists.txt` via a new `MIB_BUILD_PYTHON_BINDINGS` option
  (`cmake/MIBOptions.cmake`) rather than a separate CMake project. Required
  making `mib_processing` `POSITION_INDEPENDENT_CODE ON` (a static library
  linked into a shared `.so` needs `-fPIC`) and fixing a real bug caught by
  testing: `save_masks_to_hdf5` needs both the source image *and* the mask
  per record (`Hdf5Service::saveFrames` writes both `<group>/images` and
  `<group>/masks`); the binding originally only forwarded the mask, leaving
  `originalImage` empty and making HDF5 reject the zero-dimension dataset.
  Verified end-to-end: full pytest suite (14 tests) against a real compiled
  wheel, built three ways (editable install, `python -m build` with real PEP
  517 build isolation, and a from-scratch venv install) — all pass, plus the
  existing 48-test C++ suite and full GUI build re-verified unaffected. CI:
  `.github/workflows/python-wheel.yml` builds + tests on relevant PRs and
  publishes wheels to GitHub Releases (not a native GitHub Packages registry
  type; see `bindings/python/README.md`) on `mib-processing-v*` tags. Not
  yet auditwheel/manylinux-portable — the wheel dynamically links the same
  apt-installed OpenCV/HDF5/spdlog as `backend-ci.yml`, so a consumer's
  runtime (e.g. Biowork's `services/mib-processing` container) needs matching
  system packages, not just `pip install`.

- **Added `youngs_modulus` to the gold-standard metrics contract** (2026-07-13,
  amendment to issue #222/`contract_version: 1`) — `FilterResult::youngsModulus`
  is persisted per-frame in HDF5 metadata (`Hdf5Service.cpp`) but was missing
  from `docs/gold_standard_metrics.schema.json` entirely; surfaced while
  implementing the A3 Python bindings, whose whole point is exposing
  deformability + area + Young's modulus. Added as an **optional** field
  (schema + `docs/gold_standard_metrics.md` + `export_hdf5.py --format json`,
  omitted rather than emitted as non-JSON `NaN` when the LUT lookup misses
  coverage or an older HDF5 file lacks the field). Not yet wired into
  `compare_metrics.py`'s comparison set — left for A5, which needs to decide
  optional-field skip semantics rather than hard-failing on absence.

- **Extracted the Qt-free `mib_processing` core library** (2026-07-13, issue
  #221, part of the [Biowork portability epic
  #220](https://github.com/KPT1020/mib-studio-qt/issues/220)) — New CMake
  target `mib_processing` (`src/backend/CMakeLists.txt`) compiling
  `ProcessingService`, `EModulusLut`, `BatchMaskSources`, `Hdf5Service`,
  `FrameStore`, `Tools`, and `CrashStateMirror`, linking only OpenCV + HDF5 +
  spdlog + STL (`AUTOMOC`/`AUTOUIC`/`AUTORCC` explicitly off). `mib_backend`
  now links `mib_processing` publicly instead of compiling those sources
  itself, so the desktop app is unaffected. Severed the one real Qt leak in
  the closure: `Hdf5Service`'s optional performance-trace calls to
  `CrashReporter::capturePerformanceTransaction` (which pulls in
  `QtGlobal`/`QString` via `CrashReporter.cpp`'s `qInstallMessageHandler`)
  became an injectable `setHdf5PerformanceTraceHook`, wired to the real
  `CrashReporter` from `src/frontend/core/main.cpp` at startup — the only
  place the two are connected. `backend-ci.yml` now builds `mib_processing`
  explicitly and greps its symbols to fail CI on any Qt regression. Verified
  locally: full `linux-backend-only` test suite (47 tests) and the full GUI
  build (`linux-system-release`, `mib_studio_qt`) both pass unchanged.

- **Portable processing contract frozen (`contract_version: 1`)** (2026-07-13,
  issue #222, part of the [Biowork portability epic
  #220](https://github.com/KPT1020/mib-studio-qt/issues/220)) — Committed
  `docs/gold_standard_metrics.schema.json` (closes TD-6). Added
  `export_hdf5.py --format json`, producing schema-conformant gold-standard
  metrics JSON with a collision-safe `<h5-basename>_metrics.json` filename.
  Added `scripts/convert_legacy_csv_to_json.py` to convert legacy MIB-Studio
  metrics CSV exports to the same JSON contract, with configurable column
  names and documented defaults for fields the legacy CSV never carried
  (`area_ratio`, brightness quantiles, `touches_border`, `object_id`,
  `object_count`). Documented the `ProcessingConfig` JSON ↔ struct field
  mapping and introduced `contract_version` in `docs/gold_standard_metrics.md`
  to bundle the metrics schema, config schema, and Young's-modulus LUT format
  under one version number for portable (non-Qt) consumers.

- **HDF review export naming and batch export** (2026-07-09) -
  `HdfReviewTab` now suggests source-derived metrics filenames
  (`<h5-basename>_metrics.csv`) with collision suffixes, writes Export All
  output into source-specific folders, adds batch Metrics and batch Export All
  actions for multiple `.h5` / `.hdf5` files, remembers one shared successful
  output directory with `QSettings`, and reports per-file batch failures in a final
  summary. The standalone Python exporter and PySide wrapper now share the
  source-derived output policy: CSV-only writes `<h5-basename>_metrics.csv`,
  image/all exports write under a collision-safe `<h5-basename>/` folder, and
  `--output` remains directory-only. Added `frontend.hdf_review_export_paths`
  and `scripts.export_hdf5_paths` coverage for basename, suffix, folder, and
  output-root validation policy.

- **Realtime-performance benchmark parts C/D/E** (2026-07-02, PR1 of
  `docs/exec-plans/active/2026-07-02-realtime-performance.md`) —
  Extended `tests/performance/pipeline_timing_benchmark.cpp` (CTest
  `performance.pipeline_timing`) with three new parts proving the planned
  optimizations are real before any behavior is changed. **(C)** recording
  per-frame overhead: per-frame config lock + full-frame background clone +
  full-frame `isFrameEmpty` vs hoisted config/shared-ptr + ROI-only
  `isFrameEmpty` (1280×1024 frame, 128×128 cell ROI; measured ~4.6× speedup
  on a 24-core box). **(D)** experiment buffer trim: `vector::erase(begin())`
  at 10k-frame backlog vs `deque::pop_front()` (measured ~1500× speedup,
  proving the O(n²) steady-state cost). **(E)** snapshot publish/read
  contention: mutex-held `mask.clone()` publish vs pointer-swap with clone
  outside the mutex, under a hammering reader pool (measured ~64× reader
  throughput increase). Gates are loose and non-flaky (C: speedup ≥1.3×; D:
  ≥10×; E: reader throughput ≥0.5× legacy). No behavior change; test-only.

- **Crash-hardening: input/IO batch** (2026-07-02) — `EModulusLut` rejects
  degenerate LUT files (constant area/deform column → zero grid step →
  `size_t(floor(NaN))` UB indexing `grid_` out of bounds) and clamps lookup
  indices (new `backend.emodulus_lut_degenerate` test); `Hdf5Service` append
  paths validate batch dims against the dataset extent and series-image dims
  against the scratch buffer (heap overflow otherwise); `MainWindow`'s async
  flush captures the backend pointer instead of `this` and the destructor
  waits for an in-flight flush; `MockCamera::refreshFileList` uses the
  `error_code` `directory_iterator`; realtime loops resync a cached
  `rtLastProcessed_` that lands beyond `latest` after a `FrameStore::resize`;
  `HdfReviewTab` nav state is a `shared_ptr` instead of
  `new`/`delete`-in-connect.

- **Crash-hardening: frame buffer geometry + FrameStore identity** (2026-07-02) —
  camera buffers are validated where produced (`replenishPendingFrames`
  rejects null-base/short/garbage-size SDK buffers) and where consumed
  (`makeGrayCopy`/`makeGrayROI` in ProcessingService, the recording thread's
  strided view, and `FrameStore::getByWriteIndexROI` + AVI/TIFF exports all
  check `data.size() >= (h-1)*pitch + w` before building strided views —
  previously a pitch/size mismatch read out of bounds on the hot path).
  `FrameStore` reads also re-verify frame identity under the slot lock via a
  new `slotWriteIndices_` array, closing a TOCTOU where a wrapping producer
  (or a reader arriving before the producer's copy) returned a
  self-consistent but *wrong* frame — possibly with different geometry —
  under the requested index. Extended `frame_store_bounds_test` (pitch
  mismatch) and `frame_store_concurrency_test` (identity assertions).

- **Crash-hardening: trigger/camera stop race** (2026-07-02) —
  `CaptureService::stop()` (GUI thread) now stops the trigger thread via
  `cameraReadyCallback_(nullptr)` before `activeCamera_->stop()`, and
  `EGrabberCamera` guards every `grabber_` assignment/reset plus the
  trigger-thread `setTriggerOutput` read with a dedicated `triggerMutex_`
  (`running_` is now `std::atomic<bool>`). Previously a pending trigger pulse
  during a GUI-initiated camera stop could call into a half-destroyed
  grabber (use-after-free inside the Euresys SDK). Corrects the 2026-04-16
  thread-audit F4 assumption that camera lifecycle only runs on the capture
  thread.

- **Crash-hardening: ProcessingService exception containment** (2026-07-02) —
  worker jobs, batch workers, and the realtime loop now catch and log
  exceptions (dropping the failing job/batch or restarting the loop) instead
  of letting them escape the thread entry function and `std::terminate` the
  process on one bad frame or a throwing `cv::` call. New
  `tests/processing/processing_fault_injection_test.cpp` injects throwing
  jobs and callbacks and asserts the service keeps processing.

- **Crash-hardening: self-sufficient backend shutdown** (2026-07-02) —
  `~ProcessingService` now calls `stopRealtime()` (a joinable
  `realtimeThread_` at destruction previously hit `std::terminate`), and
  `AppBackend::shutdown()` (called from `~AppBackend`) stops capture →
  trigger → recording → processing before member destruction, closing a
  use-after-free where the realtime loop's callbacks fired into
  already-destroyed `triggerService_`/`autofocusService_` on any exit path
  that bypassed `MainWindow::closeEvent`. Regression coverage in
  `tests/backend/backend_lifecycle_smoke_test.cpp`.

- **Real-time performance examination + remediation plan** (2026-07-02) —
  Audited every component on the real-time hot path and committed
  `docs/exec-plans/active/2026-07-02-realtime-performance.md`, a 6-PR plan
  covering the remaining per-frame costs: recording-thread background
  clone/config copy + full-frame `isFrameEmpty` (P1), per-object monitoring
  clones (P2), O(n²) experiment-buffer trim (P3), redundant experiment-path
  clones (P4), mutex-held snapshot clone (P5), display-tick copies/rescale
  (P6), per-frame config/callback copies (P7), dead `FrameStore` frame-filter
  (P8), and RT thread priority (P9 → new tech-debt row TD-7). Also annotated
  the fully-implemented 2026-06-24 high-speed-capture-buffering plan
  `Status: completed`. Details in
  `knowledge_map/task/2026-07-02-realtime-performance-plan.md`. Docs-only
  change; code lands via the plan's PRs.

- **OverviewTab ROI propagation to recording** (2026-06-29) — `MainWindow`
  now connects `OverviewTab::roiChanged` to
  `ProcessingService::setRealtimeRoi()`, so the recording thread crops
  frames to the OverviewTab ROI instead of saving the full camera frame.
  The startup initialization block also seeds the processing ROI from the
  current OverviewTab values. Files: `src/frontend/core/MainWindow.cpp`.

- **HDF5 recording finalization hardening** (2026-06-26) - `Hdf5Service`
  now creates writable HDF5 files with strong-close semantics and performs an
  explicit final global flush before `H5Fclose`, logging final flush status,
  close timing, and open-object count. `AppBackend::startFrameRecording`
  only increments the recorded-frame counter after successful HDF5 appends,
  so `/recording_info` matches persisted batches. This targets stale
  superblock/EOA failures observed in recording-mode `.h5` files that required
  `h5clear --increment` repair. The per-append `.recovery.h5` full-file copy
  was **removed** (it copied the whole growing file every batch and made the
  recorder thread fall behind on NAS, dropping frames); append paths now flush
  on a time interval via `maybeIntervalFlush()` (`MIB_HDF5_FLUSH_INTERVAL_MS`,
  default 5000 ms) and there is no recovery sidecar. A mid-recording crash can
  lose up to one flush interval — the accepted tradeoff for real-time
  throughput. `recording.hdf5_resilience` covers destructor-driven
  finalization, clean-fail on a corrupted primary, and data preservation for
  recording-mode and experiment files; `recording.hdf5_save_performance` guards
  repeated-append save time.

- **Cloudflare R2 profile catalog publishing setup** (2026-06-11) — Added
  `publish-profiles.py` for KIN-47 profile catalog hosting under
  `https://updates.yofo.bio/profiles/<channel>/catalog.json`. The publisher
  validates `config_schema_version`, computes config/script SHA-256 values,
  generates upload-time `profile.meta.json`, writes `catalog.json`, and reuses
  the existing Wrangler/S3-compatible R2 credential flow. `docs/howto/auto-update-r2.md`
  now documents the required Cloudflare public bucket, cache, verification,
  and no-credential-in-repo setup for remote profile catalogs.

- **Pipeline timing benchmark** (2026-06-17) — `tests/performance/pipeline_timing_benchmark.cpp`
  (ctest `performance.pipeline_timing`) quantifies the two throttling fixes
  below. (A) Runs the shipped per-slot `FrameStore` against an in-test
  `LegacyRing` (single global mutex held across the full-frame copy) under
  1 producer + N consumers — measured ~1.6–1.8× higher full-frame-copy
  throughput on a 4-core box, and ~1.8× faster producer pushes (capture no
  longer blocked behind consumers). (B) A/Bs the new bbox/row-pointer
  brightness scan vs the old full-ROI `cv::Mat::at<>` scan, asserting the
  quantiles are **identical** across 4000 cases (~1.6× faster). Gates are
  loose (throughput ≥ 0.5× legacy; brightness identical) so CI does not flake.

- **Per-frame detection allocator/CPU cost reduction** (2026-06-17) —
  follow-up to the FrameStore fix targeting single-thread algo time in
  [[../services/ProcessingService]]. (1) `FilterResult::allContours` is now a
  `shared_ptr<const ...>` assigned once per frame instead of deep-copying the
  whole contour set into every object's result (and again into each monitoring
  / experiment copy); the write-only `hierarchy` field was deleted. (2)
  `calculateBrightnessQuantiles` now scans only the object's bounding box via
  row pointers and drops the needless `clone()` for gray input. Both costs
  previously scaled linearly with objects-per-frame — the busy/triggering case.
  Behaviour is bit-identical; covered by the existing multi-object / tracking /
  integration tests.

- **FrameStore lock-contention throttling fix** (2026-06-17) — the realtime
  image-processing and triggering pipeline was throttling because
  [[../data-model/FrameStore]] used a single `std::mutex` held *across the
  full-frame `memcpy`* on both `pushFrame` (capture) and every `get*`
  consumer (realtime loop, UI preview, raw-frame recorder). Producer and
  consumers serialised on that one lock, defeating the ring buffer's
  decoupling and stalling capture/processing/triggering. Replaced with a
  two-tier scheme: a `std::shared_mutex structureMutex_` (shared on the hot
  path, exclusive for `resize` / save / estimate) plus a per-slot
  `std::mutex` array so the copy in/out holds only that slot's lock. Also
  removed a redundant second `getByWriteIndex` of the same index in the
  realtime snapshot path (`ProcessingService::realtimeInlineLoop`) — it now
  reuses the already-fetched frame. See [[../data-model/FrameStore]]
  "Threading".

- **Experiment multi-image capture mode guard** (2026-06-16) —
  `MainWindow::onStartExperiment` now auto-switches realtime processing from
  `async_batch` to `inline` when multi-image capture is enabled, so experiment
  runs actually collect series frames that can be viewed in Review. The
  previous realtime mode is restored in `onStopExperiment`.

- **Recording review multi-image window support** (2026-06-16) —
  `writeRecordingInfo` now persists `multi_image_enabled` and
  `multi_image_count` in `/recording_info`; `HdfReviewTab` reads those
  attributes and, for recording files, loads a bounded series window from
  `/recorded_frames/images` into `FrameViewerDialog` so series navigation is
  available during review.

- **Multi-image export range selector in Review tab** (2026-06-16) —
  `HdfReviewTab` `Export All` now detects `series_images` datasets and prompts
  users to export all series frames, a custom 1-based range (for example
  `9-15`), or skip series images entirely. The export summary and logs now
  report selected series range + counts.

- **OpenAI Symphony workflow setup** (2026-06-11) - Added the repo-owned
  `WORKFLOW.md` contract for the existing Linear `mib-studio` project, plus
  `scripts/start-symphony.ps1` to bootstrap the OpenAI Symphony Elixir
  reference implementation and run it against Codex app-server. Documented the
  trusted-environment assumptions, Linear project slug, workspace location, and
  startup path in `docs/howto/symphony.md`.

- **MindVision local SDK build enablement** (2026-06-11) - Reconfigured the
  Windows Debug build for `MIB_ENABLE_MINDVISION=ON` against the installed
  MindVision SDK layout, and fixed the SDK include handling so both
  `MindVision/CameraApiLoad.h` and flat `CameraApiLoad.h` installs compile.
  The SDK dynamic-loader symbols are now owned by `MindVisionCamera.cpp`, the
  Windows `max` macro no longer breaks the boot-time
  `MIB_CAMERA_MODE=mindvision` path, and hosted Windows GitHub workflows
  explicitly keep MindVision disabled because runners do not install the
  proprietary SDK. The release workflow now builds the default target set
  before `ctest` so test executables exist when release tests run; backend
  lifecycle tests now clean temporary directories only after backend teardown.
  The Overview/Experiment tab switch path now skips EGrabber JS script
  application when a MindVision camera is selected.
- **Remote-managed Young's modulus LUT** (2026-06-11) — Added a new
  `EModulusLutCatalog` backend helper that checks a public R2 manifest,
  downloads the LUT into a user-writable app-local cache, verifies the
  SHA-256 before activation, and preserves the bundled LUT as an automatic
  fallback when offline or incompatible. `AppBackend` now logs the active LUT
  source, revision, path, and checksum status at startup, the backend-only
  build links `QtNetwork`, and the repo gained `publish-emodulus-lut.py`,
  `verify-emodulus-lut-manifest.py`, and a backend smoke test covering remote
  update + fallback behavior. Docs were updated for the LUT R2 object layout
  and cache/rollback flow.

- **Public R2-backed profile catalog + manual updates** (2026-06-11) —
  `ConfigTabs` now uses a dedicated `ProfileManager` helper to scan local
  profiles, lazily create `profile.meta.json`, fetch public catalogs on
  demand, verify SHA-256 for staged downloads, back up the previous local
  profile files, install profile updates, and surface field-level config
  diffs plus local/remote/update state in the profile row. Bundled defaults
  gained `config_schema_version` so migrated configs stay self-describing.
- **Experiment config sync hardening** (2026-06-11) — `AppConfigWatcher`
  now writes back the full supported `image_processing` config section,
  including blur, background, auto-background, target-group emodulus, and
  `multi_image` fields, then emits `configFileChanged` immediately so the
  Preview JSON editor/table and Monitoring Tune Params refresh without
  waiting for filesystem-watcher timing. `ExperimentMonitoringTab` now
  updates the histogram ring-ratio defaults through the chart-range setter,
  and the shared `JsonFlatten` utility gained round-trip helpers plus tests
  covering nested objects, arrays of objects, arrays of scalars, root
  scalar tables, and the bundled `resources/defaults/config.json`.
- **MindVision camera SDK compatibility** (2026-06-11) - Added a separate
  MindVision camera backend and discovery path alongside the existing
  EGrabber and mock workflows. CMake now exposes `MIB_ENABLE_MINDVISION`
  and fails clearly when the SDK headers/runtime DLL are missing on Windows.
  The Connect tab has a dedicated MindVision selection path, `AppBackend`
  understands `MIB_CAMERA_MODE=mindvision`, and Windows packaging copies the
  MindVision runtime DLL when enabled. Backend build/tests and docs/vault notes
  were updated to keep the non-MindVision CI path green.

- **Conan remote health precheck workflow** (2026-06-10, _removed 2026-06-23_) — Added `.github/workflows/conan-remote-health.yml`, a scheduled/manual GitHub Actions health check for ConanCenter/team-remote reachability. Removed on 2026-06-23: it was failing persistently and not providing actionable signal; the release workflow already handles remote fallback (`conan install` retries without the team remote). The release-workflow.md preflight reference was dropped with it.
- **Cloudflare R2 CI publishing path cleanup** (2026-06-10) — Both
  Windows GitHub Actions release workflows now wire stable/beta publishing
  directly through `python publish-update.py`, map R2 credentials from
  `R2_ACCESS_KEY_ID`, `R2_SECRET_ACCESS_KEY`, and `MIB_STUDIO_R2_ENDPOINT`
  into S3 environment variables, and label Cloudflare R2 steps explicitly.
  Beta manifests now target `beta/latest.json`, while stable remains
  `stable/latest.json`. `docs/howto/release-workflow.md` and
  `docs/howto/auto-update-r2.md` were updated to document this channel naming
  and secret contract.

- **Long-run experiment backlog hardening** (2026-06-02) - `ProcessingService`
  now bounds the in-memory experiment frame backlog derived from the flush
  interval, drops sampled invalid frames before valid frames when HDF5 cannot
  keep up, and exposes count-only buffered-frame stats. `MainWindow` and
  `StatsDisplayManager` use the count API so 500 ms status polling no longer
  deep-copies full OpenCV frame payloads during long experiments.

- **Backend-only Linux build/test path** (2026-06-01) - Added
  `MIB_BUILD_BACKEND_ONLY` in `CMakeLists.txt` so backend workflows can skip
  frontend executable generation. Added `linux-backend-only` configure/build/test
  presets and a `mib_backend_smoke_test` CTest target (`backend` label) for
  backend-only verification loops. Updated Linux build docs and build/run vault
  notes accordingly.

- **Crash-resilient HDF5 checkpoints for experiment/recording writes**
  (2026-06-01, **superseded 2026-06-26**) - `Hdf5Service` originally flushed
  after each append/metadata write and copied a rolling recovery snapshot to
  `<file>.recovery.h5`, with `loadFile()` falling back to it. The `.recovery.h5`
  copy was removed in the 2026-06-26 finalization hardening above (it dominated
  NAS save time and starved the recorder thread); flushing is now time-interval
  based with no sidecar. Still current from this change: recording metadata
  writes open/create existing groups/attributes instead of failing on reruns,
  and `MainWindow::onStopExperiment` flushes before writing final experiment
  metadata.

- **GUI-configurable boot disable list** (2026-05-22) - Added a Settings menu
  action (**Boot Service Toggles...**) that persists disabled startup services
  in `QSettings` (`Startup/DisabledServices`). `main.cpp` now applies that
  persisted value to `MIB_DISABLED_SERVICES` before `AppBackend::initialize`,
  so GUI choices take effect at next launch. `MainWindow` also honors
  `auto_update` at startup by skipping updater initialization and quiet checks
  when disabled. Files: `src/frontend/core/main.cpp`,
  `src/frontend/core/MainWindow.cpp`.

- **Sentry build-pipeline wiring** (2026-05-22, same branch) — Wired
  the Sentry DSN end-to-end so installed builds report crashes without
  per-machine setup. CMake gained `MIB_SENTRY_DSN` and
  `MIB_SENTRY_ENVIRONMENT` cache vars that get forwarded to ISCC; both
  `mib-studio-qt.iss` and `mib-studio-qt-update.iss` now ship
  `crashpad_handler.exe` and emit a `[Registry]` entry writing
  `HKLM\…\Environment\MIB_SENTRY_DSN` (cleanly removed on uninstall).
  `.github/workflows/build-windows.yml` injects the DSN at configure
  time from the `SENTRY_DSN` repo secret, verifies the build produced
  `mib_studio_qt.pdb` + `crashpad_handler.exe`, and runs
  `sentry-cli debug-files upload` + `sentry-cli releases new/finalize`
  using `SENTRY_AUTH_TOKEN`/`SENTRY_URL`/`SENTRY_ORG`/`SENTRY_PROJECT`
  (skips cleanly when the auth token is absent). The setup is
  documented for operators in `docs/howto/sentry-setup.md`, with the
  troubleshooting guide updated to point at the new structured crash
  artifacts under `%LOCALAPPDATA%/MIB_Studio_Qt/crashes/`.

- **Crash monitoring + remote logging** (2026-05-22, branch
  `claude/crash-monitoring-logging-jUziR`) — Added a process-level crash
  pipeline that captures Windows minidumps and a JSON snapshot of live
  service state on any unrecoverable failure (SEH, signals, uncaught C++
  exceptions, Qt fatal). New [[../services/CrashReporter]] installs the
  handlers via `dbghelp` / `std::signal` / `std::set_terminate` /
  `qInstallMessageHandler` and optionally forwards events to Sentry via
  `sentry-native` (CMake-managed clone, off by default if the fetch fails or
  `MIB_USE_SENTRY=OFF`). New [[../diagnostics/CrashStateMirror]] gives
  every service a lock-free atomic slot so the crash handler can read
  state without taking any locks; `CaptureService`, `ProcessingService`,
  `Hdf5Service`, `FrameStore`, `AutofocusService`, and `AppBackend`
  recording all write to their slots at existing lifecycle hot-spots.
  CMake now emits `/Zi + /DEBUG /OPT:REF /OPT:ICF` for Release builds so
  the produced `mib_studio_qt.pdb` can be archived for later
  symbolication via `sentry-cli upload-dif`. Crash dumps land under
  `%LOCALAPPDATA%/MIB_Studio_Qt/crashes/` and (when a DSN is configured
  via the `MIB_SENTRY_DSN` env var) pending dumps from prior runs are
  drained on next launch. Files: new `CrashReporter.{h,cpp}`,
  `CrashStateMirror.{h,cpp}`, `cmake/Sentry.cmake`; modified
  `CMakeLists.txt`, `src/frontend/core/main.cpp`,
  `src/backend/AppBackend.cpp`, and the five services above.

- **Recording HDF5 mask regeneration in Review** (2026-05-11) - The
  Review tab now keeps "Regenerate Masks" enabled for recording-mode HDF5
  files. `BatchMaskDialog` resolves the active HDF5 source dataset and uses
  `/recorded_frames/images` for recording files instead of the hardcoded
  `/valid_frames/images`, so generated recording `.h5` files can be
  remasked and reloaded as standard review HDF5 outputs. The dialog also
  has a whole-HDF5 option that processes all recording frames or both
  valid/invalid datasets for standard files, preserving source frame indices
  and writing timestamps normalised to the first regenerated image. When no
  manual background frame is selected, regeneration can synthesize a
  background by averaging the lowest-change source frames per image tile.
  Files: `HdfReviewTab.cpp`, `BatchMaskDialog.{h,cpp}`.

- **Release windeployqt Conan alignment** (2026-04-20) — `CMakeLists.txt`
  picks `windeployqt` and per-config `PATH` from `qt_PACKAGE_FOLDER_DEBUG` /
  `qt_PACKAGE_FOLDER_RELEASE` (CMakeDeps) instead of a cached `find_program`
  result and non-deterministic Conan-cache globs, fixing MSB3073 when those
  pointed at a different Qt package than the one linked for Release.

- **Recording-mode HDF5 files now open in the Review tab** (2026-04-19) —
  The "Record" button (PlaybackPanel → `AppBackend::startFrameRecording`)
  writes raw frames to `/recorded_frames/{images,metadata}` with a
  `/recording_info` group; the Review tab was hardcoded to
  `/valid_frames/*` and `/invalid_frames/*` and showed nothing.
  [[../services/Hdf5Service]] grew three reader APIs: `isRecordingFile()`,
  `readRecordingMetadata(frames)`, `readRecordingInfo(start, end, total,
  filtered)`. [[../frontend/HdfReviewTab]] now detects recording files on
  load, hides the "Invalid Frames" tab, relabels "Valid Frames" as
  "Frames", and routes all thumbnail/viewer/export reads through new
  `imagesPath(bool)` / `masksPath(bool)` helpers. Masks, overlay modes,
  ROI overlay, Export Metrics CSV and Export Charts are disabled for
  recording files (no per-frame metrics exist); Export All still writes
  TIFFs. Regenerate Masks was re-enabled on 2026-05-11. Files:
  `Hdf5Service.{h,cpp}`, `HdfReviewTab.{h,cpp}`.

- **Buffer save to AVI + AVI source for mask regeneration** (2026-04-16) —
  [[../data-model/FrameStore]] gained `saveFramesToAvi()` overloads
  (all/index-range/timestamp-range) writing a single uncompressed AVI via
  `cv::VideoWriter` — Y800 preferred, DIB/BGR fallback. Y800 files don't
  play in Windows Media Player / Movies & TV (known codec support gap)
  but play in VLC and round-trip cleanly through `cv::VideoCapture` and
  ImageJ.
  [[../frontend/Dialogs]] `BufferSaveDialog` adds an "Output Format" radio
  group — **AVI is the default** — with FPS spinner; the dialog
  auto-iterates the output path (`_1`, `_2`, ...) so it never overwrites
  an existing file or non-empty folder, and after an AVI save the
  confirmation dialog tells the user they can view the file with ImageJ
  or Fiji (no auto-launcher). [[../services/BatchMaskSources]] gets
  `loadFromAvi()` and `BatchMaskDialog` grows a third "AVI video file"
  source radio. `CMakeLists.txt` links `opencv_videoio` on both targets.
  Files: `CMakeLists.txt`, `FrameStore.{h,cpp}`, `PlaybackService.{h,cpp}`,
  `BufferSaveDialog.{h,cpp,ui}`, `BatchMaskSources.{h,cpp}`,
  `BatchMaskDialog.{h,cpp}`.

- **Periodic sort-trigger test button** (2026-04-16) — Added
  `periodicTriggerBtn` (checkable) + `periodicTriggerIntervalSpin` to
  the top row of [[../frontend/ExperimentMonitoringTab]]. When armed,
  a `QTimer` fires [[../services/TriggerService]]::`onTargetGroupResult(true)`
  every N ms (10..60000, default 1000). Interval spinbox locks while
  armed; `hideEvent` disarms. Pure UI addition; no backend changes.
  Files: `resources/ui/ExperimentMonitoringTab.ui`,
  `include/frontend/tabs/ExperimentMonitoringTab.h`,
  `src/frontend/tabs/ExperimentMonitoringTab.cpp`.

- **BatchMaskDialog always saves standard HDF5** (2026-04-16) —
  Replaced the Output group box (Display / Save PNG / Save HDF5 checkboxes)
  with a single auto-save path: `<source_dir>/<stem>_remasked.h5`. After Run,
  `HdfReviewTab` reloads via `loadHdfFile()` giving full scatter plot,
  histogram, metadata table, and thumbnail support. Overwrite is prompted.
  Files: `BatchMaskDialog.h/cpp`, `HdfReviewTab.cpp`.

- **ROI & background selection GUI in BatchMaskDialog** (2026-04-16) —
  `BatchMaskDialog` extended with a right-hand preview panel. New
  `RoiDrawCanvas` widget (`src/frontend/utils/RoiDrawCanvas.cpp`) renders a
  source frame and accepts drag-to-draw ROI selection. Frame nav buttons
  (←/→) lazy-load frames one at a time from HDF5 or folder; "Set as
  Background" captures the current frame as the subtraction background.
  ROI pre-populates from HDF5 `experiment_info` on open. `onRun()` now
  uses dialog-selected ROI + background instead of live pipeline values.

- **Batch mask generation from stream images** (2026-04-15) —
  [[../services/ProcessingService]] gained `computeProcessedFrame()` and
  `processBatch()`, enabling offline mask regeneration without driving the
  realtime loop. New [[../services/BatchMaskSources]] adapters load from
  HDF5 / folder and save as PNG / HDF5. [[../frontend/HdfReviewTab]] gets
  a "Regenerate masks…" toolbar button backed by `BatchMaskDialog`.
- **Dual syringe pump control** (PR #58) —
  [[../services/SyringePumpService]] + [[../frontend/SyringePumpTab]] +
  `SyringePumpSettingsDialog`. Modbus RTU over two COM ports (Sample +
  Sheath).
- **Multi-image recording mode** (PR #57) — [[../services/ProcessingService]]
  `multi_image_enabled`, `ProcessedFrame::seriesImages`. HDF5 gets a
  4D `series_images` dataset. [[../frontend/HdfReviewTab]] grew
  `readSeriesImagesByIndex` support.
- **Parameter tuning + monitor overlay** (PR #55) — bidirectional sync
  between the param-tuning panel and the config table; see
  [[../frontend/ConfigTabs]].
- **Ring ratio configuration and validation** — added
  `ring_ratio_min/max` + `enable_ring_ratio_check` to
  `ProcessingConfig`; [[../services/AutofocusService]] consumes the
  same values via callback.
- **Review-tab crash fix + Close File button** (PR #61) — releases HDF5
  handles cleanly; see [[../frontend/HdfReviewTab]].

## Recent fixes

- **2026-06-24** — Fixed `hardware.camera` test (`tests/hardware/hw_camera_test.cpp`)
  which asserted `isCameraConfigured()` immediately after `initialize()` with
  `MIB_CAMERA_MODE=hardware`. The EGrabber boot path (`AppBackend.cpp` ~545)
  installs the camera factory but intentionally leaves the device *selection* to
  the connect flow (so `ConnectTab` can still run discovery and pick a device),
  so `isCameraConfigured()` was `false` and the test failed before any capture.
  The test now mirrors the connect flow — when not already configured it calls
  `setHardwareCameraSelection(MIB_TEST_EGRABBER_IF, MIB_TEST_EGRABBER_DEV,
  "egrabber")` (default 0/0), matching the sibling `hardware.egrabber_script`
  test. MindVision mode is unaffected (it records its selection at boot).
  Verified on-device: captured 61 frames from an SVS-VISTEK EoSens2.0MCX12.
  Backend behaviour was deliberately left unchanged to avoid making boot-time
  hardware mode skip `ConnectTab` discovery on multi-camera rigs.

- **2026-05-05** — Made `scripts/hdf5_export.spec` and
  `scripts/build_mac.sh` portable for Unix packaging of the HDF5 Export GUI.
  `hdf5_export.spec` now resolves its script directory robustly across
  PyInstaller execution contexts (`__file__`, `SPECPATH`, fallback cwd), so
  invoking from repo root (`pyinstaller scripts/hdf5_export.spec`) works.
  `build_mac.sh` now supports both macOS and Linux: Linux builds produce
  `scripts/dist/hdf5_export_app` (ELF), while macOS still produces
  `scripts/dist/hdf5_export_app.app` with optional `--dmg`. The script also
  handles environments missing `python3-venv` by falling back to system
  Python, avoids pip self-upgrade on distro-managed Python, validates existing
  `.venv` usability, and retries dependency install in a Linux-safe way.
  Validation in cloud: `bash scripts/build_mac.sh --clean` succeeded on Linux,
  and `python3 -m PyInstaller scripts/hdf5_export.spec ...` from repo root also
  succeeded.

- **2026-04-28** — Syringe Pump Settings now supports per-pump Modbus
  baud/address configuration and in-dialog address scanning. Added
  `SyringePumpService::scanModbusAddresses(...)` and wired Sample/Sheath scan
  controls in `SyringePumpSettingsDialog` so discovered addresses can be
  applied directly and persisted to config.
- **2026-04-28** — Added a standalone raw-serial Modbus helper script for
  dLSP501 pump bring-up and manual control:
  `scripts/dlsp501_pump_minimal.py` (+ unit tests in
  `scripts/test_dlsp501_pump_minimal.py`). The script uses pyserial only
  (no higher-level Modbus package) and exposes minimal commands for
  enable/start/stop, flow+direction setup, purge, and status reads.
- **2026-04-20** — Removed obsolete `capture_processing_test` from the build.
  `CMakeLists.txt` no longer defines a `BUILD_TESTING` block for the deleted
  `src/tests/capture_processing_test.cpp` harness, and docs/vault notes were
  updated so run/build guidance now lists only `mib_studio_qt` and
  `mock_studio_qt`.
- **2026-04-20** — Made ONNX Runtime optional for Linux/cloud configure paths.
  `CMakeLists.txt` now uses `find_package(onnxruntime CONFIG QUIET)`, sets
  `MIB_HAS_ONNXRUNTIME`, and compiles `YoloService.cpp` only when the
  `onnxruntime::onnxruntime` target exists; otherwise it compiles
  `src/backend/services/YoloService.stub.cpp`. This preserves startup behavior
  (backend continues when YOLO is unavailable) while unblocking cloud builds in
  environments without packaged ONNX Runtime CMake config files.
- **2026-04-20** — Documented Linux cloud linker/toolchain workaround for
  `cannot find -lstdc++` in
  `docs/howto/mock-camera-dev-mode.md` and
  [[../build-and-run/Build]]. Root cause in affected images: `c++`
  alternative pointed to clang pathing that failed to resolve an unversioned
  `libstdc++.so` during link. Workaround:
  `sudo update-alternatives --set c++ /usr/bin/g++`, then rerun CMake.
  Validation in cloud: linker error cleared; subsequent failures were dependency
  provisioning / Conan graph issues (not compiler runtime linking).
- **2026-04-20** — Guarded Windows-only hardware SDK dependencies so Linux
  cloud builds can still compile non-hardware code paths
  (`cursor/guard-windows-deps-linux-build-fb9e`). `CMakeLists.txt` now sets
  `MIB_HAS_EGRABBER` (`ON` on Windows, `OFF` elsewhere), gates EGrabber/Coremor
  include+link paths, and compiles `AutofocusService.cpp` only on Windows
  (with `AutofocusService.stub.cpp` on non-Windows). Runtime paths now default
  to mock-camera behavior when hardware SDKs are unavailable:
  `AppBackend` forces mock mode on non-Windows and `CaptureService` default
  factory uses `MockCamera`. `CameraControlService` and `EGrabberCamera` gained
  non-Windows stubs so Linux builds no longer require EGrabber headers/libs.
  Task record:
  `knowledge_map/task/2026-04-20-linux-build-windows-hardware-guards.md`.
- **2026-04-16** — Moved autofocus statistics sort onto its own thread
  (`claude/audit-thread-performance-Pr9OI`). Follow-up to the callback
  reorder below: instead of just running ring-ratio second on the
  realtime thread, the sort is now off the realtime thread entirely.
  `AutofocusService::onRingRatio` is O(1) — a push into
  `pendingSamples_` + atomic freshness markers + `notify_one`. A new
  `statsThread_` (lifetime = service constructor → destructor) drains
  the inbox at up to 100 Hz, maintains the 1000-sample deque under
  `ringRatioMutex_`, and refreshes the `{median, average, min, max}`
  atomics. The ProcessingService realtime thread no longer touches
  `ringRatioMutex_` or the sort. Post-step buffer clear in `controlLoop`
  now also clears `pendingSamples_` under a combined `std::scoped_lock`
  so pre-step samples don't leak forward.
- **2026-04-16** — Thread performance audit + trigger callback reorder
  (`claude/audit-thread-performance-Pr9OI`). Swept every long-running
  thread for UI-thread coupling to the trigger path; the 2026-04-15 fix
  (callbacks hoisted above `monitoringFramesMutex_`) holds. Remaining
  hot-path issue: within the hoisted block, `RingRatioCallback` fired
  **before** `TargetGroupCallback`, so the [[../services/TriggerService]]
  CV wake-up was serialised behind `AutofocusService::onRingRatio`, which
  locked `ringRatioMutex_` and ran an O(n log n) sort over up to 1000
  samples (~20–50 µs per valid frame). Reordered so target-group fires
  first in all three realtime paths (ROI+drop, full+drop, every-frame).
  Task record: `knowledge_map/task/2026-04-16-thread-performance-audit.md`.
- **2026-04-15** — Trigger onset latency regression fix
  (`claude/fix-trigger-timing-bug-xGgbx`). The target-group callback
  (which wakes [[../services/TriggerService]]) was being dispatched inside
  `monitoringFramesMutex_`, the same mutex held by the UI thread when it
  snapshotted the 1000-frame monitoring ring buffer for
  [[../frontend/ExperimentMonitoringTab]] (every 500 ms). End-to-end
  trigger onset drifted from ~400 µs to up to ~1 ms at the UI cadence. Now
  the callback fires before the monitoring mutex is taken. Also removed
  per-frame `cv::Mat::clone()` on `previousFrameForAutoCapture_` (shallow
  refcounted copy is enough). Task record:
  `knowledge_map/task/2026-04-15-trigger-timing-bug.md`.
- Param tuning panel now stays in sync with the config table both
  directions.
- HDF Review tab: fixed dangling pointer crash on file close.
- **2026-04-15** — `package_installer` / `package_installer_update`
  now pass `/O${INSTALLER_OUTPUT_DIR}` to ISCC so installers land in
  `build/dist/` as CMake and `build-windows.yml` expect. Without it,
  Inno Setup honored `OutputDir=build\dist` from the `.iss` file
  relative to `resources/installers/`, writing to
  `resources/installers/build/dist/` and breaking the CI artifact
  upload step.
- **2026-04-15** — `build-windows.yml` beta path now uses
  `gh release create --target <sha>` instead of `git tag` + create.
  The local tag was never pushed, so `gh release create` refused to
  bind the release to it. `--target` makes gh create the tag
  server-side atomically.
- **2026-04-15** — `publish-update.ps1` switched from `aws` CLI to a
  small boto3 helper at `scripts/s3_upload.py`. Both CLIs omit
  `Content-Length` on `CreateMultipartUpload` and s3.yofo.bio
  rejects that; the helper registers a `before-send.s3` event hook
  that forces `Content-Length` onto every S3 request before it goes
  out. Workflows now `pip install conan boto3`. Local runs also
  require boto3 (`pip install boto3`).

## Historical tasks worth knowing about

See [[Task-Log-Index]] for the full list. Highlights:

- Safe start/stop of EGrabber (`-1012` shutdown errors).
- Preview capped at 60 Hz (configurable).
- Nanopositioner tab moved into the Config area.
- Live config reload propagates to all services.
- Config profiles (Save/Load per-user settings) — planned.
- Review system scalable to 2 GB files (lazy reads + virtualization).

## Branch context

Active development branches use the `claude/` prefix (e.g.
`claude/create-agent-onboarding-docs-J9j66`). Main is the integration
branch.
