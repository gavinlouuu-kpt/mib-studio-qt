# Implement the reviewed eGrabber-inspired MIB workspace

Status: active

Branch: `feat/egrabber-workspace`
Base: `develop` / `2a4e0a4f38055df5464400048ab9948d949319e2`

## Goal

Deliver the full Prepare / Align / Experiment / Review workflow with direct
eGrabber-style controls and linked cytometry evidence, while retaining the existing
backend, transport permissions and scientific storage semantics. Since 2026-10-14 the
same plan also carries the **experiment revamp**: experiments made of several samples
(one file per sample, recorded chapters), a calibration run, a run plan, per-sample
review and a multi-run Compare workspace ([ADR 0015](../../decisions/0015-experiment-run-chapter-identity.md),
[FORECYT.md](../../design/egrabber-workspace/FORECYT.md)). Both strands are delivered
through the single sequence in **Delivery sequence** below. The complete
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
  - Add **P7 Compare** (multi-run workspace) to this plan. *(Withdrawn 2026-10-15, see below.)*
  - Not adopted: electronic signatures / 21 CFR Part 11, and plate/rack formats until
    an autosampler device class exists.
  - Amended the same day: pass/fail-only bead QC is not adopted, but a **calibration
    run** is. It is a run of a reference sample whose results become an applied,
    instrument-scoped calibration record (see P2 and ADR 0015 item 7).
- 2026-10-15: Scope trim after review ("follow good software's logic"). The workspace
  records and shows truthfully; analysis belongs to the analysis tool. Therefore:
  - **Removed from this plan:** P7 Compare (owned by #314 ComparisonSpec and #523 FJ09),
    feature-range "criteria sliders" (multi-criteria gating is #523 FJ04; the shell keeps
    polygon AND E), the calibration history chart (a plain list suffices), auto-revert
    of hardware overrides (keep the banner and the snapshot record), and the
    method + analysis-plan "recipe" pairing.
  - **Deferred:** chapters (`mark_chapter`, `/chapters`, `chapterId`) until a mid-run
    sample-change experiment is actually scheduled; ADR 0015 item 3 stays decided but
    unimplemented. The sample queue ships as one file per sample only.
  - **Simplified:** the sample sheet is a fixed small schema (sample, condition, unit,
    concentration + unit, notes, free key/values), not a layer system; the queue is a
    guided UI flow over existing stop/finalize/start commands plus `experiment_id` /
    `sample_ref` in the start request, not a backend queue; snapshot v3 drops the
    free-text channel/medium/temperature fields (they are annotation).

- 2026-10-15 (owner): the operation app needs **simple, instant analysis** so a
  researcher can compare runs and refine the experiment while it is still running —
  the iQue-style loop — but not FlowJo-depth analysis, which stays in the Review app.
  Scope, called **Quick look**: descriptive per-run numbers (n, median area /
  deformability / E, % in the polygon-AND-E population, event rate, loss flags) computed
  by the backend from accounting plus a bounded pass at close, overlaid against an
  operator-chosen reference run with the delta on medians; no tests, fits or replicate
  claims; it leads to *Repeat sample / Adjust next / Continue / Open in Review*. The
  Review app recomputes authoritatively. Both apps share **one UI language**: shell,
  numbered stage tabs, toolbar, card grid, chips, vocabulary, condition colours, the
  Area–Deformability chart with E curves and the record-identity display; only the
  brand differs.

## Delivery sequence

Work packages P0–P7 describe *scope*. This table is the *order*: each slice is one or a
few PRs against this branch, with its own tests, vault and manual updates. A slice may
start when its "needs" are merged. Backend-first slices (S2, S3) unblock the UI slices;
the UI slices are safe to build against the checked-in contract before then as long
as missing data stays Unavailable.

| # | Slice | Package | Needs | Delivers | Gate |
|---|---|---|---|---|---|
| S0 | Hygiene fixes | — | nothing | #711 pumps stop on last-client loss; `ReviewPanel` header comment; readiness probe caching (#713 part); FCS `$MIB_RUN_ID`; `WORKFLOW.md` recent-work rule | unit tests beside each fix |
| S1 | Decisions and views | P0 | nothing | ADR 0015; this plan; FORECYT.md; proposed views 00–06 in the fold-in sandbox file (**done 2026-10-15**, PR #710; views 05-criteria and 06-Compare are now reference-only) | `check_docs.py` |
| S2 | Run snapshot v3 | P4 (backend) | ADR 0015; ABI number reserved with the coordinator | #712: `experiment_id` / `run_index` / `sample_ref`, `/experiment_annotation` (fixed schema + free key/values), E-LUT id/sha, pump/LED/stage readbacks, selected operator, calibration record id; PZ `runId`/`epoch` persisted; start request carries annotation and stop condition (objects / duration / both, backend-owned, reason recorded) | round-trip, legacy read (absent ≠ fabricated), fault injection |
| S3 | Shell and Prepare | P1, P2 | S0 | opt-in four-stage workspace; backend gates shown in Prepare with Unknown/Unsupported (#713); extended confirmation signature; storage budget gate and run-plan card; fluidics shown as estimates; non-default-settings banner (no auto-revert) | P1 and P2 gates |
| S4 | Calibration run | P2 (backend + UI) | S2, S3 | calibration method purpose; record derivation in the backend (size → pixel→µm proposal; offset and E-vs-nominal check-only); apply with previous vs new; age/identity readiness gate per method policy; record list | derivation tests on synthetic bead runs; gate invalidation; no silent method change |
| S5 | Experiment Plan, Live and Quick look | P4 (UI + backend quick-look pass) | S2, S3 | Plan sub-view (format: single / queue; sample sheet, fixed schema); guided queue flow over existing commands (stop → drain → finalize → close → prompt → readiness → Start); lifecycle stepper; separate stop actions; detected-cell strip and scatter click-to-pin in React; **Quick look**: backend-computed descriptive per-run numbers and a reference-run overlay in the run summary and the experiment board | P4 gate; quick-look numbers reconcile with the Review app on the same file; a lost controlling client pauses the flow and never resumes it (S0) |
| S6 | Single review | P5 | #512 merged; S2 | Studio mounts the shared review; full u64 event identity; polygon AND E; per-run statistics and small multiples **consumed from** the #523 worker (FJ05/FJ08), not re-implemented in the shell | P5 gate |
| S7 | Parity and promotion | P6 | S3–S6 | all 37 + 5 views reviewed; host and PZ7035 runs measured; manual, release notes, screenshots | P6 gate; promotion by normal review, CI and hardware acceptance |

Who does each slice: the work is split into an Operation app track (#716), a Review app
track (#717) and the shared kit (#715); see
[WORK-SPLIT.md](../../design/egrabber-workspace/WORK-SPLIT.md).

Deferred (not scheduled here): chapters (ADR 0015 item 3; needs `mark_chapter`, `/chapters`,
`chapterId`, review ABI 2); multi-run Compare and dose-response (#314, #523 FJ09);
feature-range criteria (#523 FJ04); recipe pairing.

Rules that apply to every slice:

- Contracts are additive; ABI numbers are reserved before the PR opens (ADR 0004).
- Nothing per-sample is shown as real until S2/S3 have landed; before that the UI shows
  Unavailable, not an inferred value.
- New mutating commands (`mark_chapter`, calibration apply, queue control) join the
  bridge server allowlist with tests.
- PZ7035: S4–S6 run on the instrument UI; S7–S8 are PC-side (YOFO Review / desktop),
  the instrument shows a run summary only (#501).

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
- **Status 2026-10-19:** the opt-in workspace shell is merged (#727, on `@mib/ui` via #723) with
  the no-actuation navigation gate; the classic route stays the default. The capability
  inventory is not written yet.

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
  - A plain list of past records (applied / superseded / invalidated); no trend chart.

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
- **Status 2026-10-19:** Prepare shows the backend gates without a path, with Unknown/Unsupported
  and the extended confirmation signature (#727, #713); the probe is cached (#719). Storage
  budget, run-plan card, fluidics estimates, the non-default banner and the calibration run are
  not started.

### P3 — Align and direct device controls

- Persistent image with fit/1:1/zoom/pan, pixel probe, histogram and line profile.
- Typed quick controls backed by the same scoped feature service as All Features.
- Pending write/readback, bounds/increment handling, write-only/RO/unavailable states,
  safe selector enumeration and configuration invalidation.
- Retain all GenApi modules, advanced visibility/search/XML, scripts and engineering
  tools. Provider-specific features are capability-gated.
- Gate: scoped readback races, reject/timeout cases, selector restoration, image
  inspection does not block capture, parity with existing configuration scripts.
- **Status 2026-10-19:** fit/1:1/zoom/pan, pixel probe, histogram, line profile and the ROI quick
  control (requested → readback) are merged (#729). Writable exposure/gain/frame rate, selector
  enumeration and restoration, and S2 overrides are not started.

### P4 — Acquisition, retained history and recording

- Separate source-start acknowledgment, first-buffer observation, conversion,
  processing and writer status; distinguish deliberate discard from actual loss.
- Draft/apply buffer capacity; retention time/memory; multipart identity; generation
  invalidation; bounded copied evidence; previous/next/exact-index playback.
- Independent supported recording modes with truthful mutual exclusions; stop,
  drain, finalize/close/error/retry; reconciled accepted/written/dropped/pending counts.
- Exact export range, progress, verify, cancel/failure and source preservation.
- Experiment → Plan sub-view (ADR 0015): sample format (single sample or sample queue;
  time segments deferred with chapters; rack/plate hidden unless an autosampler
  capability is reported), a sample sheet with a fixed small schema (sample, condition,
  experimental unit, concentration + unit, notes, free key/values), one colour per
  condition, per-run auto-stop option, and "base on previous experiment".
- Keep the live inspection features operators already rely on, in the React shell:
  a strip of the most recently detected cells (bounded owned copies, explicit
  eviction, click to pin) and click-a-scatter-point-to-see-the-cell during a run
  (pinned selection with prev/next and an open-in-window viewer; the copy survives ring
  eviction; no image → metadata only with the reason). Today this exists only in the
  Qt Monitoring tab (2026-10-12); the React Monitoring view has no per-row image
  command yet. Reference views 02 and 21.
- Sample queue as a guided flow over existing commands: Next sample = stop → drain →
  finalize → confirmed close, prompt, readiness, then Start of the next entry. No
  backend queue state; nothing resumes on its own. Lifecycle stepper.
  Non-default-settings banner (requested vs readback; recorded, never auto-reverted).
- Run snapshot v3: experiment/run/sample identity, planned annotation, E-LUT id/sha,
  pump/LED/stage readbacks, selected operator. Persist PZ `runId`/`epoch`.
- Quick look (instant, descriptive): per run n, median area / deformability / E, % in
  the polygon-AND-E population, event rate and loss flags, computed by the backend
  (monitoring accumulators live; a bounded full-file pass at close, cached with the
  run) — never in the shell or the worker. The experiment board shows the queue's runs
  side by side with these numbers and an overlay of contours against an
  operator-chosen reference run, with the delta on medians. No tests, fits or
  replicate statements; the Review app recomputes authoritatively on the same file.
  Decisions it feeds: Repeat sample · Adjust next sample · Continue · Open in Review.
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
- Population = polygon AND E interval, with a live "N of M (x %)". Multi-criteria
  gating is the analysis tool's job (#523 FJ04) and is not built in the shell.
- Per-run / per-population statistics and small multiples are consumed from the #523
  worker (FJ05/FJ08) with inline flags (low n, loss in run, E outside the LUT domain,
  legacy file); the shell does not compute statistics.
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

## Acceptance criteria

- [ ] Operator can Prepare hardware/software/storage, Align, inspect live cells,
  select a population, record, finalize, reopen and export without ambiguous state.
- [ ] Operator can plan an experiment of several samples and run them as a queue (one
  file per sample) with per-run auto-stop; every file carries the identities and
  annotation that let the analysis tool group and compare them (ADR 0015).
- [ ] A calibration run produces a record that is applied only by explicit operator
  action, is referenced by later runs, and gates readiness per the method's policy.
- [ ] Losing the controlling client leaves fluidics, stage, recording and the queue in a
  safe, non-resuming state (#711).
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
