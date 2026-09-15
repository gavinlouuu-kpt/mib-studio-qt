Title: Reliability release — make every host-camera run safe, accountable, reproducible (epic #371)

Date: 2026-09-06 → 2026-09-07

Context
- Epic #371 coordinates the Host SDK reliability/data-integrity release:
  #365 #366 #360 (lifecycle), #367 (accounting), #368 (time/telemetry
  truth), #369 + host part of #274 (readiness + run snapshot + bounded
  background), #344 (exporter), #363 #358 #359 #361 #362 #364 (truthful,
  bounded UI), #370 (byte budgets). Out of scope: #349 FPGA Direct, #246
  React/Tauri, Contract v2 (#296/#303), new science.
- Working principles applied throughout: no wholesale rewrite; explicit
  typed outcomes instead of booleans; unknown telemetry never rendered as
  0; unsupported state never Ready/Verified; every code change with its
  vault note; deterministic, watchdog-guarded tests; sanitizer lanes where
  supported.

Delivery order (one commit per phase, branch
`claude/host-sdk-reliability-qt-ui-g03ubd`)
1. `788ae83` #365 #366 — capture lifecycle single owner, MindVision
   fail-closed conversion; `6c52bc0` #360 — one camera command path.
2. `a9bdacb` #367 — frame accounting + FrameStore commit boundary.
3. `5136106` #368 — timestamp semantics + per-metric telemetry validity.
4. `28efc1e` #369/#274 — readiness transaction, immutable run snapshot,
   bounded background calibration.
5. `51bb8b9` #344 — exporter streaming/lifecycle + 50-run soak.
6. `e984b3c` #358 #359, `da8feee` #361 #362, `60566f2` #363, `921f48d`
   #364 — viewport-safe layout, single-owner sidebar, explicit config edit
   state, run state / alerts / metrics separation with async finalization,
   Monitoring tune panel with acknowledged apply.
7. `62ca332` #370 — byte-budgeted ownership, bounded retention,
   presentation counters; this note + release evidence.

Evidence
- `docs/evidence/2026-09-07-reliability-release-371/README.md` — criteria
  → implementation → guards matrix, lane results, checklist, open items.
- `docs/evidence/2026-09-07-exporter-soak/` (#344),
  `docs/evidence/2026-09-07-memory-budget/` (#370).
- Per-phase notes: [[../current-state/Recent-Work]] entries dated
  2026-09-06/07; architecture notes [[../architecture/ExperimentCoordinator]],
  [[../diagnostics/MemoryBudget]], [[../services/HdfExportService]].

Open after this release
- Hardware acceptance on the Windows bench (MindVision / EGrabber,
  packaged executable) — every guard here is hardware-free by design.
- Long hardware soak with exact final accounting; the equations are
  enforced on every mock run.
