# MonitoringDensityService

> Backend-owned live population density (KDE) of the Monitoring scatter and
> its core contour, computed so that it can never compete with acquisition.
> Every shell (Qt today, React/Tauri next) pushes settings and reads the
> same result.

**Source:** `src/backend/services/MonitoringDensityService.cpp`,
`include/backend/services/MonitoringDensityService.h`; kernel and record
codec `include/backend/processing/MonitoringDensity.h`,
`include/backend/processing/KdeCoreRecord.h` (Qt-free, header-only).
**Related:** [[ProcessingService]], [[../architecture/ExperimentCoordinator]],
[[../architecture/AppBackend]], [[../frontend/ExperimentMonitoringTab]],
[[../data-model/HDF5-Storage]]

## Responsibility

- One worker thread at the **lowest OS priority** (`SCHED_IDLE` on Linux,
  `THREAD_PRIORITY_LOWEST` on Windows): it runs only on a core that would
  otherwise idle, so capture, processing, recording and the trigger always
  win. Under full load the contour goes stale instead of costing frames.
- Each tick (every `intervalMs`, 500–60000, default 2000, or on
  `requestUpdate()`), in this order:
  1. disabled → nothing runs;
  2. **load back-off** — `underLoad(now, before)`: frames dropped since the
     last tick (batch queue + experiment buffer drops) or the batch queue at
     least a quarter full → the tick is skipped (`skippedUnderLoad`);
  3. **unchanged input** (count + first/last frame index + µm factor +
     bandwidth factor + core fraction + grid range) → skipped;
  4. estimate: Gaussian KDE at every cell (per-axis Silverman bandwidth ×
     factor; one pairwise pass gives the raw densities and their maximum,
     which normalises both the points and the grid), core level =
     `ceil(p·n)`-th largest density, 128 × 64 **separable** grid
     ((128+64)·n `exp()` calls), marching-squares contour;
  5. **compute budget** — the next wake is at least
     `kComputeBudgetFactor` (20) × the last estimate's **thread CPU time**
     (`CLOCK_THREAD_CPUTIME_ID` / `GetThreadTimes`), so the duty cycle stays
     ≤ ~5% of one core on any host. Wall time is only reported
     (`wallMs`, `lastWallMs`): a starved idle-priority worker waits without
     using CPU and must not stretch its own interval.
- Cost in the running app (Linux container, real 512x96 stream, 200 fps):
  ~90 ms CPU per 1000-cell estimate, next wake ~1.8 s (was ~600 ms before
  the separable grid and the one-pass normaliser).
- Input: `AppBackend` copies `ProcessingService::getMonitoringValidPoints()`
  (index, area, deformability and the row's identity only — no image
  references under the ring lock) and converts area to µm² with the current
  pixel-to-micron factor. Since ABI next (#886, set at merge) each point carries its whole row
  identity, `MonitoringDensityKey{startGeneration, captureEpoch, frameIndex,
  objectId}` (the ring slot's `MonitoringRowTag` plus the frame's index and
  object id), because a frame index alone names nothing: it restarts with
  every capture, the ring holds rows of several captures and runs, and a
  frame can hold more than one object. The keys run parallel to the indices
  (`MonitoringDensityInput::keys` / `MonitoringDensityResult::keys`, all or
  nothing: a provider that cannot name every row sends none), and the
  unchanged-input fingerprint includes the first and last key, so a new
  capture that restarts the numbering is never mistaken for the same input.
- Output: `latest()` (shared, immutable `MonitoringDensityResult`) and a
  monotonic `generation()`; shells poll the generation. Disabling drops the
  result (re-enable recomputes); an empty ring drops a stale result.
- Record: after each estimate the provisional core record
  (`liveRecordJson`, `provisional: true`, `source: "live-buffer"`) goes to
  the record sink, wired to
  `ExperimentCoordinator::setLiveKdeCoreRecord` — kept only while a run is
  `Active`, written at finalization. No shell is involved.

## Key APIs

`setSettings(MonitoringDensitySettings)` (clamped by
`MonitoringDensitySettings::clamped()`: interval 500–60000, factor 0.2–5,
core fraction 0.05–1, a non-finite factor or fraction falls back to its
default, axes that are not all finite become 0; `enabled`,
`intervalMs`, `bandwidthFactor`, `coreFraction`, grid range `x0..y1` — the
shell's chart axes, empty = padded data range), `settings()`,
`requestUpdate()`, `latest()`, `generation()`, `busy()` (pending or
computing), `stats()` (`estimates`, `skippedUnchanged`, `skippedUnderLoad`,
`busyMs`, `lastComputeMs`, `nextIntervalMs`, `priorityLowered`), `stop()`.
Pure policy for tests: `nextIntervalMs`, `underLoad`, `compute`,
`liveRecordJson`.

## The bridge surface (ABI next (#886, set at merge), #866 PR E)

The service was already shared by every shell; ABI next (#886, set at merge) gives the Rust/Tauri
and WebSocket shells the same reads and writes the Qt tab had
(`bridge-contract.json` sections `monitoring_density`,
`kde_reference_from_file`, `emodulus_lut`; full shapes in
[[../architecture/Rust-Bridge]]):

- `monitoring_density_set_settings` (control: VIEWER_ONLY on the WebSocket
  server, like `monitoring_set_active`) → `{ok, message, settings}` with the
  clamped settings as the service applies them. The frontend sends
  `enabled: false` while the sub-tab is hidden; disabling drops the result.
- `monitoring_density_request_update` (control) wakes the worker now; a
  no-op while disabled.
- `fetch_monitoring_density(sinceGeneration)` (read) answers
  `{valid, generation, unchanged: true}` (one atomic read) when nothing is
  newer, else the result: a density per point (rounded, `null` when not
  finite), the points named by `(start_generation, capture_epoch,
  frame_index, object_id)` through a small `contexts` table so the payload
  stays at most 1000 rows, the contours, the grid and the stats the Qt
  tooltip shows (`refresh_ms` = max(interval, next wake)). The frontend
  matches a point to a snapshot row by the whole identity; a row newer than
  the estimate has no density yet.
- `fetch_kde_reference_from_file(path)` (read) reads a reference core contour
  from an experiment file: the stored full-run record first, the live one
  second, Qt's reason wording, at most 20000 vertices
  (`readKdeReference`). It never opens the file in Review. HDF5 is built
  without thread safety (#722), so the read goes through the lanes' lock and
  the session mutex (`ReviewJobs::readKdeReference` →
  `ReviewSession::readKdeReference`) inside the coordinator's non-waiting
  idle transaction with no raw recording writing, like the run quick look;
  while an experiment runs or starts the call is refused with plain text
  (Qt reads with a reader of its own and does not refuse; the frontend loads
  the reference while idle and keeps the contours in memory).
- `fetch_emodulus_lut(knownIdentity)` (read) returns the loaded E-modulus
  table once per identity (`"<sha256>|<path>"`), `unchanged` otherwise.

No command computes anything: the estimate stays on the worker, a command
takes the service mutex for a few field reads and one `shared_ptr` copy,
and nothing was added to the acquisition or processing path.

## Lifecycle

Constructed in the `AppBackend` constructor (callbacks tolerate services
that `initialize()` has not built yet), idle until a shell enables it,
stopped first in `AppBackend::shutdown()` — before the coordinator is
finalized — and declared after the coordinator so it is destroyed first.
The Qt Monitoring tab enables it only while the tab is visible, mirroring
the visibility-gated monitoring ring.

## Tests

- `backend.monitoring_density_service` — policy, clamping, disabled no-op,
  first estimate + record, fingerprint skip, interval-only change, fraction
  and axis changes, load back-off (drops, backlog), disable/re-enable,
  empty ring, prompt stop, four concurrent callers (TSan lane); row identity
  (keys per point, a new capture with the same indices re-estimates, a new
  run, an object-id change in the last row, no keys → none returned).
- `e2e.monitoring_density_facade` — the bridge facade: settings echo and
  clamping, the result's identity (two captures, the 1000-point bound,
  disable), the reference read (full-run, live, no record, missing file,
  refused during a run), the E-modulus table (identity, unchanged), stats
  telemetry.
- Rust `contract.rs` `monitoring_density_is_described`,
  `monitoring_density_end_to_end`; `dispatch.rs`
  `monitoring_density_through_dispatch`; `mib-bridge-server`
  `monitoring_density_settings_are_control_only_but_reads_are_not`;
  `monitoringDensity.test.ts` (the TS decoders).
- `performance.monitoring_density_contention` — duty cycle within the
  budget when idle; real `computeProcessedFrame` on every core, density off
  vs on in alternating windows, median throughput ratio ≥ 0.90; starved
  worker's next wake bounded by its CPU cost, not its wait.
- `e2e.experiment_coordinator` — a run where only the service supplies the
  record: the finalized file carries it.
- `processing.monitoring_density` — kernel invariants; separable grid and
  one-pass normaliser equal the direct sums at a fraction of the cost.
- `integration.monitoring_kde_e2e` (real `MainWindow`, mock camera,
  Hugging Face frames) — capture/processing/lag ratios, estimates land,
  interval + budget kept, lowest priority, stored record.

The Qt end-to-end fixture uses the pinned `512x96stream-mock-frames` asset
when available (`MIB_KDE_E2E_FRAMES` can point at another folder). Without the
asset it writes deterministic noisy-grey frames with bright Contract 1
ellipses that survive Contract 1's saturating current-background subtraction,
so the synthetic path still exercises detected cells. The test is registered on
Windows with `integration;e2e;frontend;monitoring;performance`: the
`build-windows` performance-filtered lane and the fast Windows presets exclude
it, the integration presets select it, and the release CTest invocation is
unfiltered so it includes the test.

## Gotchas

- Historical polarity check (`v1.1.2`, `54211084`, `7c274c26`): the default
  live mask uses Contract 1 saturating subtraction at all three revisions.
  The original intensity-70 synthetic cells on a 140 background are therefore
  incompatible with that default; a historical real-asset pass does not prove
  a synthetic-fallback pass. The exact later test also references `lastWallMs`,
  absent at `54211084`, so it cannot validate that endpoint unchanged.

- The input is the **processing monitoring ring**, not a shell's display
  buffer; tests inject with `ProcessingService::appendMonitoringFrameForTests`.
- The ring only fills while monitoring is active (`setMonitoringActive`),
  so no estimate — and no stored record — exists for a run watched from
  another tab.
- Settings persistence still lives in the shell (Qt `QSettings`
  `Monitoring/Kde*`); the bridge now exposes the service (ABI next (#886, set at merge)), so the
  React shell keeps its own copy of the settings and pushes them (the
  service holds one set per instrument, shared by every client).

The #595 KDE e2e harness waits for captured/monitoring frame work and two KDE generations rather than fixed phase durations. Its 120 s progress deadlines and 900 s CTest timeout accommodate parallel CI load. Work per captured frame, overlay lag, GUI responsiveness, compute-budget cadence, real contours, persisted live records and disabled-KDE recovery remain gated; wall-time FPS is diagnostic.
