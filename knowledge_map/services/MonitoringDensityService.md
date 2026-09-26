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
     factor, normalised to [0, 1]), core level = `ceil(p·n)`-th largest
     density, 128 × 64 grid, marching-squares contour;
  5. **compute budget** — the next wake is at least
     `kComputeBudgetFactor` (20) × the last compute time, so the duty cycle
     stays ≤ ~5% of one core on any host.
- Input: `AppBackend` copies `ProcessingService::getMonitoringValidPoints()`
  (index, area, deformability only — no image references under the ring
  lock) and converts area to µm² with the current pixel-to-micron factor.
- Output: `latest()` (shared, immutable `MonitoringDensityResult`) and a
  monotonic `generation()`; shells poll the generation. Disabling drops the
  result (re-enable recomputes); an empty ring drops a stale result.
- Record: after each estimate the provisional core record
  (`liveRecordJson`, `provisional: true`, `source: "live-buffer"`) goes to
  the record sink, wired to
  `ExperimentCoordinator::setLiveKdeCoreRecord` — kept only while a run is
  `Active`, written at finalization. No shell is involved.

## Key APIs

`setSettings(MonitoringDensitySettings)` (clamped; `enabled`,
`intervalMs`, `bandwidthFactor`, `coreFraction`, grid range `x0..y1` — the
shell's chart axes, empty = padded data range), `settings()`,
`requestUpdate()`, `latest()`, `generation()`, `busy()` (pending or
computing), `stats()` (`estimates`, `skippedUnchanged`, `skippedUnderLoad`,
`busyMs`, `lastComputeMs`, `nextIntervalMs`, `priorityLowered`), `stop()`.
Pure policy for tests: `nextIntervalMs`, `underLoad`, `compute`,
`liveRecordJson`.

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
  empty ring, prompt stop, four concurrent callers (TSan lane).
- `performance.monitoring_density_contention` — duty cycle within the
  budget when idle; real `computeProcessedFrame` on every core, density off
  vs on in alternating windows, median throughput ratio ≥ 0.90.
- `e2e.experiment_coordinator` — a run where only the service supplies the
  record: the finalized file carries it.
- `processing.monitoring_density` — kernel invariants.

## Gotchas

- The input is the **processing monitoring ring**, not a shell's display
  buffer; tests inject with `ProcessingService::appendMonitoringFrameForTests`.
- The ring only fills while monitoring is active (`setMonitoringActive`),
  so no estimate — and no stored record — exists for a run watched from
  another tab.
- Settings persistence still lives in the shell (Qt `QSettings`
  `Monitoring/Kde*`); a React shell needs its own until the bridge exposes
  the service (follow-up).
