# Fold-in realignment: mechanism first (E, I, F)

Status: active

## Goal

The three fold-ins (#806 eGrabber, #807 iQue/Forecyt, #808 FlowJo) deliver the
underlying mechanism, not only a presentation of it. A capability counts as
folded in only when a native authority (C++ service or Rust bridge code) does
the work on real data, the authority is reachable through a versioned
facade/bridge command, the UI calls that command, and a test drives the real
authority. This plan records the 2026-11-02 audit of every open fold-in branch,
the drift already visible, and the slices that close each gap.

Tracking: #806, #807, #808 (coordination overlays); PRs #809, #812 (E),
#811, #813, #814, #815 (I), #810 (F).

**Anchor:** the per-capability backend goals (E01–E09, I01–I12, F01–F16) live in
[`docs/architecture/fold-in-backend-goals.md`](../../architecture/fold-in-backend-goals.md).
That file is the backend counterpart of the Penpot boards. Every slice below names
the goal IDs it advances and the headless proof it adds.

## Definition of done: applies to every capability ID

A ledger row (E01–E09, I01–I12, F01–F16) may be marked accepted only when all
five layers exist. A row missing any layer is reported with the missing layer
named, never as "done", "integrated" or "restored".

1. **Authority.** Native code computes or actuates. TypeScript may format,
   lay out and hit-test; it never computes a scientific number, a readiness
   verdict or a device state.
2. **Contract.** A facade/bridge command with an agreed ABI number and
   contract text. One owner per ABI number (see D2).
3. **Wiring.** The production view calls the command. Fixtures are imported
   only by tests and by labelled dev demos (`*-demo.html`).
4. **Real-backend test.** At least one test drives the native authority
   (ctest, `crates/*/tests`, or the WS acceptance scripts), not a mocked
   bridge.
5. **Evidence class.** Fixture / real backend / hardware stated per row, at
   realistic scale (see F-1 for analysis).

A slice that adds only layer 3 over a fixture is permitted as a design probe,
but its PR title and ledger row must say "UI probe, no authority".

## Audit findings (2026-11-02)

Branch heads audited: `feat/806-egrabber-fold-in`, `feat/806-typed-features`,
`feat/807-ique-workflow`, `feat/807-ique-qc-trends`, `feat/807-ique-recipes`,
`feat/807-ique-workspace-core`, `feat/808-flowjo-realignment`. No production
view was found presenting fixture numbers as real data, and every ledger says
0 accepted. The problems are missing authority, scale, duplication and narrative.

| Track | Mechanism built | Mechanism missing |
|---|---|---|
| E | E01, E03–E06 real and wired (pre-#806 services); #809 adds a discovery reply guard and two real-backend tests; #812 adds a sound typed-feature module | #812 has **no caller** and no bridge command, so exposure/gain/FPS/trigger, selectors and categories do not work end to end (E02). No GenApi enumeration anywhere (see E objective). No reconnect/session generation, chapters, multipart identity, firmware/version report. No hardware runs. |
| I | #815: SQLite `WorkspaceStore` (revision CAS, history, restore, receipts), `workspace_read/write` through facade, bridge, Tauri; `NextRunPlanning`; native tests. #813 plots real calibration records (read-only) | #811 is presentation only; #814 is demo-only with an unimplemented `RecipeProvider`. No authority for I03 estimates, I07 metric definitions, I08 hit finding, I10 fits, I11 thresholds/exclusions. Recipe compatibility always "unknown". |
| F | Native gates/population DAG with tri-state logic, statistics, overlays, comparison, reports; SQLite persistence; supervised Cytoflow/sklearn worker; review bridge to UI | Hard caps of **10,000 events / 64 MiB**; sources passed as JSON row arrays, so a real recording cannot be analysed. FlowJo 10.8.1 reference (229,585 / 190,705 events) never runs through the native evaluator. No FCS/GatingML (F15), compensation, Logicle, inference/fits. UI is mostly JSON textareas. |

## Drift already happening

1. **I is building views and assuming F supplies the engine.** Three of four
   I PRs are TypeScript only. The ledger credits I07/I09 to a "pinned F native
   authority", but that code arrived by a squash copy of F (item 2), and no
   I-owned view or criterion evaluator was built on it. Until I08/I07/I11
   have native criteria, metric and threshold authorities, I is a dashboard
   over nothing new.
2. **I and F own the same files twice.** `6fc2f926` on #815 is a squash copy
   of the #808 analysis stack (~130 files), and #810 copied I's
   `WorkspaceStore` back. The copies diverged; `git cherry` finds no shared
   patches. Both branches claim **bridge ABI 47 / review ABI 9 with different
   contents**. #810 is based on `develop`, 144 commits behind
   `feat/egrabber-workspace`.
3. **F's production engine changed without a decision.** #808 records
   Cytoflow as the production engine behind a worker adapter. #810 evaluates
   every population natively and uses Cytoflow only for a polygon parity
   check, and uses sklearn (not Cytoflow) for t-SNE. This may be the right
   design, but it is an engine decision and needs an owner decision (D3).
4. **F scale is prototype scale.** All native proofs use synthetic sources of
   4–22 events. The ledger's "two-recording production backend milestone
   passes" claims more than this supports.
5. **E is at risk of becoming a curated feature list.** See E objective.
   `AlignView.tsx` tells the operator "All features, the GenApi XML and
   scripts stay in the classic view", but no code in `src/`, `crates/` or
   `desktop/src/` enumerates GenApi features or categories or reads the
   device XML. The only GenApi access is by caller-named feature
   (`EGrabberCamera.cpp`, and #812's `EGrabberFeaturePort::describe`). The
   pointer leads to nothing.
6. **Narrative overclaims.**
   - #815's `docs/design/ique-workflow/README.md` still says no shared host,
     build or ABI files were edited, but the ABI went 44→47.
   - PR titles say "restores QC trends" and "recipe pairing" for a read-only
     history and a demo-only card.

## E objective (clarified, binding for #806)

The binding statement is the E track goal and E02 in the
[backend goals](../../architecture/fold-in-backend-goals.md). The notes below
expand it.

The purpose of the eGrabber fold-in is to **work with the camera through
GenApi as the device describes itself**, not to maintain a list of features.

- The feature tree, categories, types, access mode, visibility, units,
  ranges, increments and enum entries come from the connected device's
  GenApi node map at runtime, through the eGrabber introspection queries
  (feature and category enumeration, `query::info`, `implemented`,
  `readable`, `writeable`, `enumEntries`). Verify exact query names
  against the installed SDK headers before use.
- No hand-maintained list, JSON or XML of feature names drives what the UI
  shows or allows. Quick controls (exposure, gain, FPS, ROI, trigger) are
  *bindings* to standard SFNC names, each resolved against the live node map.
  If the node is absent or not writable, the control shows that state.
- Access is re-read after every write and after selector changes, because
  GenApi access depends on other nodes. Selector exploration restores the
  original selector value.
- The device XML may be exported for diagnostics. It is never parsed into a
  private schema that replaces the node map.
- Per-camera knowledge (e.g. "this model needs `LineSelector` before
  `TriggerSource`") lives in code next to the GenApi call, documented as a
  device quirk, not as a feature catalog.

## Gap-closing slices

Each slice is one reviewed PR with the five layers above.

### D: decisions first (owner)

- **D1:** Accept the definition of done above as the ledger rule for #806,
  #807 and #808.
- **D2:** Analysis code and ABI 47 / review ABI 9 belong to F (#810). I
  rebases onto it; next free ABI numbers are allocated in #716 before a
  slice uses them.
- **D3:** F engine. Decided: the native evaluator is the production authority
  and Cytoflow is a comparator ([ADR 0016](../../decisions/0016-native-analysis-authority.md)).
- **D4:** Accept the E objective above as part of #806.

### Untangle (blocks I and F review)

- **U-1:** Sync `develop` into `feat/egrabber-workspace`, then rebase #810
  onto it. The bridge ABI stays at 47 and the review ABI at 9.
- **U-2:** Rebase #815 onto #810's head. Drop `6fc2f926`'s copied files and
  keep only I-specific commits (`WorkspaceStore` extensions,
  `NextRunPlanning`, `SavedPlan`, `FindingNextRun`). Re-number its contract
  additions per D2. Correct the I README and ledger rows I07/I09.

### E: eGrabber

- **E-1 GenApi enumeration (E02 core).** Extend #812's `Port` with
  enumeration of features and categories per module and info fields
  (display name, tooltip, visibility). Add a backend service that enumerates
  the opened grabber's node map under the existing session guard. Real-backend
  test: the fake SDK port returns a node map, and the service returns exactly
  that tree. No hard-coded names.
- **E-2 Wire typed control.** Call `FeatureControl` from `EGrabberCamera`/
  `CaptureService` under the session guard. Add bridge commands
  `camera_feature_tree`, `camera_feature_read`, `camera_feature_write`
  (ABI number from #716). The session token carries the source generation so
  stale replies are refused.
- **E-3 Quick controls as SFNC bindings.** Exposure, gain, FPS and trigger in
  Align call E-2 by SFNC name and show requested / readback / refused /
  unsupported. Remove "set in classic view" text for anything now writable.
  Fix the "All features" pointer to open the E-1 browser, or drop it until
  the browser exists.
- **E-4 Reconnect generation (E01).** Bump the generation on open and
  reconnect; refuse replies from older generations in backend and UI. Test
  the backend-restart case the ledger documents.
- **E-5 Hardware qualification.** Windows eGrabber rig: run the E01–E06
  completion scenario and attach logs. Scope evidence for trigger and pulse
  claims.
- Later: chapters (E06), multipart/channel identity (E03),
  firmware/version report (E07/E08), identity contract with F (E09).

### I: iQue / Forecyt

Every I slice adds a native authority or consumes an existing one through
the bridge. No I slice is TypeScript only.

- **I-1 Recipe authority (I02).** Implement `RecipeProvider` in native code
  over the existing method registry and #815's `WorkspaceStore`: immutable
  pairs, base-on-previous drafts, and a real compatibility diff (method
  revision, profile, core, calibration). Mount `RecipePairing` in production.
- **I-2 Criteria and hit finding (I08).** Criteria definitions are stored as F
  analysis-plan populations. Hit count and denominator come from F's
  evaluator through the bridge, with the declared missing-value policy.
  Sliders edit a draft and commit through the authority. No evaluation in
  TypeScript.
- **I-3 Metric definitions (I07).** A native store of named metric
  definitions (population × parameter × statistic) and a validated expression
  grammar evaluated by F's statistics code. Quality flags (low n, missing
  calibration, loss, out of domain) come from the result, not the view.
- **I-4 QC thresholds and exclusions (I11).** Versioned thresholds and
  reversible exclusions as calibration-record annotations in the backend.
  Trends read verdicts computed against the threshold revision. Old runs are
  never recalculated silently.
- **I-5 Worklist estimates (I03).** A backend estimator for time and storage
  from method, frame size, rate and free space, with stated assumptions and
  freshness. Readiness stays the coordinator's.
- **I-6 Comparison and fits (I09, I10).** After F-4, an I view selecting runs
  by annotation over F's `ComparisonSpec`, plus a fit authority (see F-4).
- **I-7 Annotations drive results (I01).** Annotation groups flow into F
  grouping, colours and exports through the workspace document, not view
  state.

### F: FlowJo

- **F-1 Scale (blocker).** Remove the 10,000-event and 64 MiB caps. Read
  HDF5 columns in chunks inside the native evaluator, return membership as a
  handle or bitset (not JSON rows), and fingerprint by streaming. Acceptance:
  a real MIB recording of at least 200k events gates, counts and reports
  within a stated memory bound.
- **F-2 Reference through MIB code.** FCS 3.x import (F15 read side) feeding
  the native evaluator. The FlowJo 10.8.1 fixture must reproduce 229,585 /
  190,705 through `PopulationGraph`, not a standalone Python recount.
- **F-3 Engine decision applied (D3, ADR 0016).** Update the #808 ledger and
  docs to name the native authority. Keep Cytoflow parity tests in CI and add
  a FlowKit comparator on the reference fixture. Record t-SNE's sklearn
  version in the ledger.
- **F-4 Inference and fits (F09, I10).** Declared experimental unit,
  pairing and weighting, the model, and suspect-fit reasons, in native code
  or the worker, with independent numeric fixtures.
- **F-5 Gate editing UI (F04).** Replace JSON textareas with drawn
  polygon/ellipse/range gates that commit through the existing authority.
  No new science.
- **F-6 Interchange (F15).** FCS export and supported GatingML with
  declared units, unsupported constructs and transforms. Compensation and
  Logicle (F03) where applicable to MIB channels.

## Sequencing

1. D1–D4, then U-1 and U-2.
2. Run in parallel: F-1 → F-2 (F track), E-1 → E-2 → E-3 (E track), and
   I-1 (I track).
3. I-2 and I-3 after F-1 (they need the evaluator at real scale). I-4 and
   I-5 can run at any time.
4. F-3 to F-6, E-4, I-6, I-7.
5. E-5 hardware qualification and the integrated acceptance in #806–#808.

## Decision log

- 2026-11-02: **D1–D4 decided** (owner delegated the decisions):
  - **D1 accepted.** The five-layer definition of done is the ledger rule for
    #806, #807 and #808. `.github/workflows/fold-in-goal-guard.yml` enforces
    the visible part: a fold-in PR must name a goal ID, and a fold-in PR that
    changes only views must say "UI probe".
  - **D2 accepted.** F (#810) owns `src/backend/analysis`,
    `crates/mib-analysis-worker`, the review analysis bridge and the bridge
    ABI 47 / review ABI 9 contract text. I (#815) rebases onto #810 and
    drops `6fc2f926`'s copies. I's own contract additions take the next free
    numbers, allocated in #716 before use.
  - **D3: native authority.** See [ADR 0016](../../decisions/0016-native-analysis-authority.md).
    The native C++ analysis code is the single production authority.
    Cytoflow and FlowKit are CI comparators and are not bundled. The worker
    stays for qualified long jobs (t-SNE) with permissive dependencies. This
    replaces #808's "Cytoflow as production engine".
  - **D4 accepted.** The E objective is GenApi node-map driven (E track goal
    and E02 in the backend goals).

- 2026-11-02: Every objective is written as a backend goal with a headless
  proof, in `docs/architecture/fold-in-backend-goals.md`. A view is built only
  after its goal is met; anything earlier is a labelled UI probe and counts for
  nothing. Reason: Penpot anchors the UI, but nothing anchored the backend, so
  agents drifted into building views.
- 2026-11-02: Audit of all fold-in branches. Adopted mechanism-first
  definition of done. Clarified the E objective as GenApi-driven node-map work,
  not a feature list. Identified I/F duplication of analysis code and ABI 47.

## Progress

- [x] D1–D4 recorded (2026-11-02; ADR 0016; fold-in goal guard)
- [ ] U-1, U-2
- [ ] E-1, E-2, E-3, E-4, E-5
- [ ] I-1, I-2, I-3, I-4, I-5, I-6, I-7
- [ ] F-1, F-2, F-3, F-4, F-5, F-6
