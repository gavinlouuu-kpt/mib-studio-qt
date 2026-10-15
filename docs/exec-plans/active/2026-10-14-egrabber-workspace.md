# Implement the reviewed eGrabber-inspired MIB workspace

Status: active

Branch: `feat/egrabber-workspace`
Base: `develop` / `2a4e0a4f38055df5464400048ab9948d949319e2`

## Goal

Deliver the full Prepare / Align / Experiment / Review workflow with direct
eGrabber-style controls and linked cytometry evidence, while retaining the existing
backend, transport permissions and scientific storage semantics. The complete
[design brief](../../design/egrabber-workspace/README.md) is normative; the
[reviewed v2 prototype](../../design/egrabber-workspace/prototype.html) specifies the
seven later interaction refinements, not a replacement seven-tab production app.

## Decision log

- 2026-10-14: Start from current develop; do not fork from an older tutorial snapshot.
- 2026-10-14: Keep the working Qt and React paths intact during staged migration.
- 2026-10-14: The first commit is design/reference scaffolding only. No runtime
  behavior, bridge ABI, dependencies, hardware configuration or protection rules change.
- 2026-10-14: Use the existing AppBackend / BackendFacade and transport abstractions.
  Missing bridge features remain explicitly unavailable until implemented and tested.
- 2026-10-14: Keep simulation outside desktop production assets/routes. Never import
  its fabricated telemetry, E function, update versions or success toggles into live code.
- 2026-10-14: Native Penpot navigation remains unverified. This does not block code
  implementation against the checked-in design contract, and is not reported as fixed.
- 2026-10-14: Fold in lessons from plate-based high-throughput cytometry software
  ([design note](../../design/egrabber-workspace/FORECYT.md)) as content for the existing
  four stages, not as a parallel navigation. Owner decisions, recorded in
  [ADR 0015](../../decisions/0015-experiment-run-chapter-identity.md):
  - An experiment is a set of runs. A manual-swap sample queue records **one file per
    sample**. Chapters mark sample changes inside one run and are recorded at
    acquisition, never inferred.
  - The planned sample annotation is written immutably into each run at Start.
    Corrections live in the workspace store (#397/#523) and never rewrite the recording.
  - Auto-stop is a per-run option: manual (default), target valid-object count, duration,
    or both (whichever first). The backend owns it.
  - Per-population statistics, multi-run comparison and fits run in the #523 Cytoflow
    worker. The GPL licence gate remains #523's.
  - Add **P7 Compare** (multi-run workspace) to this plan.
  - Not adopted: electronic signatures / 21 CFR Part 11, and plate/rack formats until
    an autosampler device class exists.
  - Amended the same day: pass/fail-only bead QC is not adopted, but a **calibration
    run** is. It is a run of a reference sample whose results become an applied,
    instrument-scoped calibration record (see P2 and ADR 0015 item 7).

## Work packages and completion gates

### P0 — Versioned reference (this setup)

- [x] Create the isolated feature branch from the pinned develop commit.
- [x] Record complete workspace scope and approved behavioral constraints.
- [x] Preserve a public-safe copy of the reviewed v2 interaction prototype.
- [x] Add dependency-free reference smoke checks and source hashes.
- [ ] Production UI implementation (intentionally not part of P0).

### P1 — Capability audit and opt-in shell

- Inventory real bridge DTOs/commands for source, acquisition, preflight, motor,
  piezo, LED, storage, updater, playback and event evidence. Record supported,
  unsupported, stale and unknown states; reuse existing behavior rather than resetting it.
- Implement an opt-in React workspace with Prepare / Align / Experiment / Review,
  stable context and acquisition toolbar, image area and contextual tools. Keep the
  existing route as a regression baseline; no silent startup change.
- Share backend-derived workflow/preflight/quality/control-role state. Navigation
  must not start capture, home a motor, enable light or install an update.
- Gate: React tests + TypeScript/Vite build; resize/keyboard review; identical backend
  command log before and after pure navigation; normal shell remains available.

### P2 — Instrument preparation and failure recovery

- Integrate source discovery/open progress and stage-specific error/retry handling.
- Add LED/controller and coarse-motor/fine-piezo preparation with real capabilities,
  explicit requested/readback values, limits and safe test authorization.
- Extend storage status through the bridge when absent: path, capacity reserve,
  predicted session size and explicit idle write qualification.
- Expose software compatibility vs optional update information; preserve updater
  integrity checks, platform restrictions, consent and restart/requalification handling.
- Required checks gate only the appropriate operations, and invalidate on changed
  device/profile/configuration identity or stale readings. Show cause and recovery.
- Show the authoritative backend readiness gates in Prepare (today they run only inside
  Start, after the file pick). Add Unknown/Unsupported to the frontend checklist.
  Extend the confirmation signature with PL build/weights, pump/stage identity, profile
  id and storage path. Cache the path writability probe instead of writing a file on
  every evaluation.
- Calibration run (ADR 0015 item 7). A method with purpose *calibration* records a
  reference sample through the normal run path. The backend derives a calibration
  record:
  - measured size against the reference's nominal value, to check or propose
    pixel-to-micron;
  - the deformation offset of rigid references;
  - event-rate stability and focus;
  - E against the nominal value where the reference has one.

  The record is versioned and instrument-scoped (#393). It references its source run
  and the background / dot-grid results. Operation rules:
  - It is a proposal until an operator applies it, with previous vs new values shown.
    It never silently changes a method.
  - A readiness gate checks the age and validity of the applied record. Each method
    sets the policy: none, warn or required. The record is invalidated by chip,
    objective, camera or ROI identity changes.
  - A history view of records with spec lines and reversible exclusions.

  Decided 2026-10-14 (owner delegated):
  - Reference material: rigid size beads of known diameter are the standard
    calibration sample. Stiffness references are optional and check-only.
  - Only pixel-to-micron may be *applied*, as a proposal the operator confirms.
    Deformation offset and E-vs-nominal are reported, never subtracted or corrected;
    a science correction belongs in a processing contract (ADR 0007).
  - Expiry: hard invalidation on chip / objective / camera / ROI identity change; soft
    age warning, default 24 h, policy per method (none / warn / required).
  - Derivation runs in the C++ backend. Measurements come from the normal processing
    core; the record feeds a readiness gate and a processing input, so it stays out of
    the review worker.
- Run plan card: stop condition × rate × payload → estimated duration and size. Use
  measured SSD fresh vs sustained rates on the PZ7035. Sample volume is shown as an
  estimate, never as a level.
- Gate: pure derivation tests, mocked transport UI tests, backend faults, no-actuation
  startup/navigation test; Windows/Linux and remote viewer restrictions.

### P3 — Align and direct device controls

- Persistent image with fit/1:1/zoom/pan, pixel probe, histogram and line profile.
- Typed quick controls backed by the same scoped feature service as All Features.
- Pending write/readback, bounds/increment handling, write-only/RO/unavailable states,
  safe selector enumeration and configuration invalidation.
- Retain all GenApi modules, advanced visibility/search/XML, scripts and engineering
  tools. Provider-specific features are capability-gated.
- Gate: scoped readback races, reject/timeout cases, selector restoration, image
  inspection does not block capture, parity with existing configuration scripts.

### P4 — Acquisition, retained history and recording

- Separate source-start acknowledgment, first-buffer observation, conversion,
  processing and writer status; distinguish deliberate discard from actual loss.
- Draft/apply buffer capacity; retention time/memory; multipart identity; generation
  invalidation; bounded copied evidence; previous/next/exact-index playback.
- Independent supported recording modes with truthful mutual exclusions; stop,
  drain, finalize/close/error/retry; reconciled accepted/written/dropped/pending counts.
- Exact export range, progress, verify, cancel/failure and source preservation.
- Experiment → Plan sub-view (ADR 0015): sample format (single sample, sample queue,
  time segments; rack/plate hidden unless an autosampler capability is reported),
  sample sheet with typed annotation layers and one colour registry, per-run auto-stop
  option, and the method + analysis-plan pairing.
- Keep the live inspection features operators already rely on, in the React shell:
  a strip of the most recently detected cells (bounded owned copies, explicit
  eviction, click to pin) and click-a-scatter-point-to-see-the-cell during a run
  (pinned selection with prev/next and an open-in-window viewer; the copy survives ring
  eviction; no image → metadata only with the reason). Today this exists only in the
  Qt Monitoring tab (2026-10-12); the React Monitoring view has no per-row image
  command yet. Reference views 02 and 21.
- Sample queue: Next sample = stop → drain → finalize → confirmed close, then Start
  of the next entry after its readiness. Time segments: `mark_chapter` with boundary
  accounting. Lifecycle stepper. Non-default-settings banner (requested vs readback).
- Run snapshot v3: experiment/run/sample identity, planned annotation, E-LUT id/sha,
  pump/LED/stage readbacks, selected operator, declared or unknown channel, medium and
  temperature. Persist PZ `runId`/`epoch`.
- Gate: storage round-trip + fault-injection, lifecycle e2e + concurrency stress,
  latency/invariant tests, TSan if threads/shared state change, slow/full/disconnected disk.

### P5 — Linked cytometry review

- Event -> exact image/part/object/mask/measurements; record table and scatter selection
  synchronized; loading/expired/not-recorded/error states and stale-response rejection.
- Raw/contour/mask modes, pinned live inspection and saved-run review retain provenance.
- Polygon AND optional E interval using validated method/calibration LUT; preserve
  Area-Deformability axes; distinguish inspecting, drawing and editing.
- Separate stored results from explicit reprocessing; do not invent tracking.
- Precondition: one review implementation in Studio and YOFO Review (#512). Studio's
  Review tab does not mount `review/ReviewPanel.tsx` today.
- Populations: polygon AND E interval AND optional feature-range criteria, with a live
  "N of M (x %)" and the inspected record's values marked. Analysis-only unless
  explicitly deployed to a sort.
- Per-run / per-chapter / per-population statistics (Cytoflow worker) with inline
  flags (low n, loss in chapter, E outside the LUT domain, legacy file). Small multiples
  per run, chapter or annotation group. Chapter review flags but never moves boundaries.
- Gate: rapid/out-of-order selection, u64 IDs, multiple objects and parts, per-record
  availability, LUT validity/out-of-domain, unchanged gating on inspect, reopen/export round-trip.

### P6 — Parity, performance and promotion

- Review all 37 named wireframe views, including multi-source/multi-bank, firmware,
  scripts, diagnostic tools, licenses and unavailable provider capabilities.
- Exercise representative workflows on host and FPGA-backed platforms and the
  supported operating systems. Record actual measurements rather than demo rates.
- Update manual screenshots, vault notes and acceptance evidence in each slice.
- Submit focused PRs against this integration branch where useful; promotion into
  develop requires the repository's normal review, CI and hardware acceptance.

### P7 — Compare (multi-run workspace)

- Open several runs grouped by experiment and annotation. Incompatible runs are listed
  with the reason (chip, LUT, processing contract, metric definition), following #314
  ComparisonSpec.
- Live by default; a frozen snapshot only for reports.
- Dose-response and summary statistics on per-experimental-unit summaries, with declared
  unit, pairing and weighting (#314, #523 FJ09). Suspect-fit reasons inline; no
  automatic interpretation.
- PC-side for the PZ7035 (instrument shows a run summary only, #501).
- Gate: compatibility-rule tests, legacy-file (unchaptered, no annotation) handling,
  reproducible fits from exported summaries, worker cancellation and stale-result
  rejection.

## Acceptance criteria

- [ ] Operator can Prepare hardware/software/storage, Align, inspect live cells,
  select a population, record, finalize, reopen and export without ambiguous state.
- [ ] No unbridged capability is presented as verified; no simulated hardware data
  enters production code; permissions are enforced by backend as well as UI.
- [ ] Existing working shell and backend regressions stay green during migration.
- [ ] Reference behavior tests and real application tests are separately identified.
- [ ] Scientific identities, counts, timestamps and analysis provenance reconcile.
- [ ] Usability review covers target desktop size, narrower view, keyboard and
  non-overlapping labels/toolbars; mobile preview is not a hardware qualification.

## Verification commands

For the checked-in reference:

```bash
node --test docs/design/egrabber-workspace/prototype.test.mjs
```

For later application slices (after the repo doctor/environment setup):

```bash
python3 scripts/check_docs.py
cd desktop
npm ci
npm test
npm run build
```

Backend/save/lifecycle slices additionally run the required CTest, fault, stress,
sanitisers and latency checks from the root agent guide. No full native build or
hardware test is claimed by this design-baseline commit.
