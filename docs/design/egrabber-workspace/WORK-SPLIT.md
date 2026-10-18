# Two tracks: Operation app and Review app

Status: active, 2026-10-18. Progress, 2026-10-19:

- **A0:** done. #711 merged in #718, and #713's probe caching in #719.
- **A1:** done. #723 wires `@mib/ui` into the app; #727 adds the opt-in workspace and Prepare
  (backend gates read without a path, Unknown/Unsupported, required warnings still block per
  #548, one verdict, extended confirmation signature). One backend test remains on #713.
- **A2:** in review: #725 (snapshot v3 file schema) and #728 (start-request identity and
  `/experiment_annotation`, ABI 39), both on `develop`.
- **A3:** first PR merged (#729): Align's live image tools and ROI quick controls; navigation and
  leaving the workspace never switch the camera mode. Writable exposure, gain and frame rate need
  a backend feature service (a later P3 step); the kit NumberField is #730.
- **B0:** #512 S1–S4 and S9 merged (#721, #724, #726, #731). All nine #512 decisions are ruled on
  #717; D1 keeps Start refused while a review file is open (HDF5 thread safety, #722). S5 is in
  progress.
- **Kit:** DataTable, CellImage, OptionGroup and Notice are in #714; NumberField is requested (#730).

The workspace delivers two products from one UI language
([README](README.md), "One UI language, two products"). Their work is split into
two tracks and one shared kit, so the tracks can run in parallel without editing
the same files. The order of slices is still the [delivery
sequence](../../exec-plans/active/2026-10-14-egrabber-workspace.md#delivery-sequence);
this note says who does each slice.

| Track | Product | Owns | Tracking issue |
|---|---|---|---|
| A | Operation app: MIB Studio on the desktop, YOFO Studio on the PZ7035 | `desktop/src/**` except `desktop/src/review/**`; `index.html`; the run-writing backend | #716 |
| B | Review app: YOFO Review on a PC, and the review module Studio mounts | `desktop/src/review/**`; `review.html`; review packaging; the review contract | #717 |
| Kit | `@mib/ui`, used by both | `desktop/ui/**` (PR #714) | #715 |

Workspace slices target `feat/egrabber-workspace`, where the docs, desktop, bridge and
backend checks run on every pull request. Fixes that every build needs
(S0, #512) and contract changes that bump an ABI (#712, #512 S10–S12) target `develop`, which
the workspace branch then merges: ABI numbers are assigned in order on the trunk, and the
coordinator reserves each one before its PR opens. A track never
edits another track's directories. Files both tracks touch (`desktop/package.json`, `desktop/vite.config.ts`,
`desktop/src-tauri/`, `desktop/src/bridgeContract.ts`) change in small separate PRs,
merged one at a time.

## How the three tools divide each view

Every view goes through the same loop. The kit is the source of truth for components:
Penpot and Claude Design meet only through it.

1. **Penpot** holds the reviewed board. Every O and R board below already exists on the
   "Operation app" and "Review app" pages of the fold-in sandbox file
   (ids in [PENPOT.md](PENPOT.md)).
2. **Claude Design** prototypes the flow with the real kit (project
   "MIB · YOFO UI language"). It does not invent components. When a prototype needs one,
   the request goes to #715. Each board's band names its track step and
   "Claude Design prototype: pending"; the accepted prototype replaces "pending".
3. **The kit** gets any missing component: its `C · <Name>` board on the Penpot page
   "Components (@mib/ui)" first, then code, then a Claude Design re-sync.
4. **The track** implements the view in its own directories, with tests, vault and
   manual updates in the same PR.

A view can be implemented before its missing kit pieces exist: it keeps its current
app-local markup until they land.

## Track A: Operation app (#716)

| Step | View | Slice | Needs | Missing kit pieces |
|---|---|---|---|---|
| A0 | none | S0 hygiene (#711, #713 probe caching) | nothing | none |
| A1 | O1 Prepare | S3 shell and Prepare (#713) | A0 | Notice |
| A2 | none (contract) | S2 run snapshot v3 (#712), on `develop` | ABI 39 reserved | none |
| A3 | O2 Align | P3; overrides recorded by S2 | A1 | Notice |
| A4 | O1 calibration card | S4 calibration run | A1, A2 | none |
| A5 | O3 Plan | S5 | A1, A2 | DataTable, OptionGroup |
| A6 | O4 Live | S5 | A5 | CellImage |
| A7 | O5 Run summary, O6 Quick look | S5 with the backend quick-look pass | A6 | none |

## Track B: Review app (#717)

| Step | View | Slice | Needs | Missing kit pieces |
|---|---|---|---|---|
| B0 | none | One review implementation (#512) | nothing | none |
| B1 | R1 Open | Review shell on the kit (#269) | B0 | DataTable |
| B2 | R2 Overview | S6; statistics from the #523 worker | B1, A2 | DataTable |
| B3 | R3 Inspect | S6 | B1 | DataTable, CellImage |
| B4 | R6 Export | Parity (#269) | B1 | OptionGroup, DataTable |
| B5 | Studio mount | S6 | B3 | none |

R4 Populations (#523 FJ04) and R5 Compare (#314, #523 FJ09) remain reference views
and are not scheduled.

## Kit (#715)

| Component | Needed by | Order |
|---|---|---|
| DataTable | O3, R1, R2, R3, R6 | 1 |
| CellImage | O4, R3 | 2 |
| OptionGroup | O3, R6 | 3 |
| Notice | O1, O2 | 4 |

`AreaDeformChart` already covers small multiples (R2, through `width`/`height`) and the
reference overlay (O6, through per-point condition slots).

## Where the tracks meet

These are the only interfaces; anything else is a sign that work belongs to one track.

- **The run file.** Track A writes run snapshot v3 (#712), and track B reads it.
  Changes are additive with reserved ABI numbers. Legacy files read as absent,
  never fabricated.
- **Quick look.** O5/O6 numbers must equal the Review app's on the same file. Track A
  owns the shared fixture test, and track B agrees the fixture.
- **The review module.** Track B exports one entry point. Track A mounts it as Studio's
  Review stage (B5) and passes Studio-specific needs as props, never a fork.
- **The kit.** Both tracks consume `@mib/ui` and request changes through #715.

## Claude Design briefs

One prototype per view, built with the kit only and illustrative data. Each brief names
the flow to try, not the layout, which the Penpot board already fixes.

| View | Prototype |
|---|---|
| O1 | Readiness from blocked to ready: a gate turns from Unknown to Pass, then Start is enabled. |
| O2 | Change two quick controls; the "settings differ from method" notice appears with requested vs readback. |
| O3 | Fill a three-sample sheet, choose queue, and see the queue preview with duration and size. |
| O4 | A queue run moving through the lifecycle stepper; click a detected cell to pin it. |
| O5 | A closed run's outcome, counts and provenance, then Next sample. |
| O6 | Reference-run overlay with median deltas, then Repeat, Adjust or Continue. |
| R1 | Open an experiment from the library; legacy files show what is absent. |
| R2 | Per-run statistics with flags and small multiples per condition. |
| R3 | Select a point; the record, image and stored vs reprocessed values follow. |
| R6 | Choose exports, preview the metrics file, and follow the export job to done. |
