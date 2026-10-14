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
- Gate: storage round-trip + fault-injection, lifecycle e2e + concurrency stress,
  latency/invariant tests, TSan if threads/shared state change, slow/full/disconnected disk.

### P5 — Linked cytometry review

- Event -> exact image/part/object/mask/measurements; record table and scatter selection
  synchronized; loading/expired/not-recorded/error states and stale-response rejection.
- Raw/contour/mask modes, pinned live inspection and saved-run review retain provenance.
- Polygon AND optional E interval using validated method/calibration LUT; preserve
  Area-Deformability axes; distinguish inspecting, drawing and editing.
- Separate stored results from explicit reprocessing; do not invent tracking.
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
