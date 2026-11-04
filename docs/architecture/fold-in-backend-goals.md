# Fold-in backend goals (E, I, F)

**This file is the backend anchor for the three fold-ins.** Penpot boards anchor
what the UI looks like. This file anchors what the backend must *do*. The UI is a
manifestation of these goals. A view whose goal below is unmet shows an empty
or unsupported state; it does not stand in for the goal.

Tracks: eGrabber [#806](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/806),
iQue/Forecyt [#807](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/807),
FlowJo [#808](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/808).
Execution and status:
[`../exec-plans/active/2026-11-02-fold-in-mechanism-realignment.md`](../exec-plans/active/2026-11-02-fold-in-mechanism-realignment.md).

## Rules for every agent on E, I or F work

1. **Name the goal.** Every PR states which goal IDs below it advances and
   which proof it adds or extends. A PR that advances no goal is not fold-in
   work.
2. **Backend before view.** A view for a capability may be built only after
   its backend goal is met. Until then a view counts as a *UI probe*: label it
   as one in the PR title and ledger, keep it out of production routes, and
   count it as nothing toward acceptance.
3. **The headless test.** Each goal is met only when its proof runs with no UI.
   That means a native test, a `crates/*/tests` test, or a script against the
   bridge/WS server (like `tools/*_acceptance`). It must use real backend code
   on realistic data. If deleting the views would lose any state or number,
   that state or number lives in the wrong layer.
4. **No science, verdicts or device state in TypeScript.** Views format, lay out,
   hit-test and edit drafts. Native authorities compute, validate, decide and
   persist.
5. **Goals change only by owner decision**, recorded in the plan's decision log.
   Narrowing a goal to fit what was built is drift. Write down the gap instead.

## E: eGrabber fold-in

**Track goal.** MIB drives a GenICam camera **through the camera's own GenApi node
map**:
- it discovers sources and recovers from failure with generation-checked
  identity;
- it enumerates, reads and writes features exactly as the device describes them;
- it acquires independently of display and recording;
- it retains and records frames with conserved accounting;
- it hands exact frame identity to analysis.

No hand-kept list, JSON or XML of feature names decides what exists or is
writable.

| ID | Backend goal (true when done) | Authority | Proof |
|---|---|---|---|
| E01 | Discovery distinguishes missing producer, busy and disconnected. Every open/reconnect increments a source generation carried on every reply. The backend refuses replies and commands from an older generation, including after a backend restart. | `DeviceDiscoveryService`, camera session | Native test: restart that reuses job ID, stale reply refused. Rig log of the three states. |
| E02 | The backend enumerates the live node map per module: categories, features, type, access, visibility, unit, min/max/increment, enum entries. It reads and writes any feature by name, re-reads access after every write and selector change, and returns requested / readback / refused / unsupported / unknown-outcome. Quick controls (exposure, gain, FPS, ROI, trigger) are bindings to standard SFNC names resolved against that map, not a list. | `FeatureControl` (#812) behind `EGrabberCamera`/`CaptureService`, bridge feature commands | Fake-port node map round trip with no names hard-coded. On the rig, the enumerated tree for one camera matches the vendor tool's tree for the same camera. |
| E03 | The backend serves raw frames with pixel format, part/channel identity and the applied display transform. Probe, histogram and profile values come from raw pixels, not the rendered canvas. | Capture / FrameStore read path | Known-image fixture through the real capture path: values correct after zoom, ROI and part changes. |
| E04 | Acquisition, preview and recording are independent backend state machines. Preview failure never changes writer state. "Start accepted" and "first frame observed" are separate events. | `CaptureService`, `RecorderService`, coordinator | Pipeline fault injection on the preview path, with the writer unaffected. |
| E05 | Retention capacity and reserve are configured in the backend. Eviction is counted. Reallocation follows safe stop → reallocate with no auto-resume. Retained frames play back exactly by ID. | `FrameStore`, preview buffer, `PlaybackService` | Real-server tests: resize refusal (exists), eviction count, exact playback by ID. |
| E06 | The recorder lifecycle (open/start/stop/drain/finalize/closed/error) has reconciled counts. Disk-full, cancel, disconnect and failed close never report "saved". Exported ranges reopen. Chapters are added when the writer contract adds them. | `RecorderService`, `Hdf5Service`, `HdfExportService` | Fault-injection tests plus a reopen-and-count test. |
| E07 | The backend computes readiness for LED, focus, storage qualification and firmware/software compatibility, with unknown, unsupported and fail kept distinct. Start is gated by that verdict alone. Actuation happens only on explicit commands. | `ExperimentCoordinator` readiness | One headless test per readiness state; no actuation during discovery or navigation. |
| E08 | Scripts, diagnostics and the version/capability report are backend commands with idle and authorization gates enforced in the backend. | Camera script service, diagnostics | Refusal tests while acquiring; capability report for mock and rig. |
| E09 | Every frame/object reference carries source, run, frame, object and processing/calibration revisions as lossless values. The resolver returns pixels or a reason. | EvidenceResolver contract | Round trip of a reference through the resolver, including the expired case. |

## I: iQue / Forecyt fold-in

**Track goal.** The experiment is a backend object. Native authorities store,
validate and compute all of the following:
- plans, samples, annotations and recipes;
- worklist estimates and run identity;
- metrics, criteria, hits, comparisons and fits;
- QC verdicts and findings.

Those authorities are the coordinator, the workspace store, the method registry
and F's evaluator. The I views edit drafts and display what those authorities
return. **Deleting every I view loses no experiment state and no number.**

| ID | Backend goal (true when done) | Authority | Proof |
|---|---|---|---|
| I01 | Annotations are stored with a schema: condition, experimental unit, concentration+unit, notes and extensible groups. The backend exposes the grouping that F results and exports use. Cell count is never the replicate count. | `WorkspaceStore`, F grouping | Headless: write annotations → grouped F statistics and export carry the same groups. |
| I02 | Recipes are immutable pairs of an acquisition method revision and an analysis plan revision. Base-on-previous creates a new draft. The compatibility diff (method, profile, core, calibration) is computed in the backend. Apply is explicit, with no implicit hardware change. | Native `RecipeProvider` over the method registry and `WorkspaceStore` | Headless create → copy → diff → apply; the original stays unchanged. |
| I03 | The backend estimates time, storage and resources per worklist item, stating assumptions and freshness and naming unsupported sensors. The coordinator owns readiness. | Worklist estimator | Estimate vs measured run within stated tolerance. |
| I04 | The coordinator queue writes one file per sample and locks identity at start. Retries are idempotent by request ID, so a disconnect or retry never duplicates or mislabels a run. | `ExperimentCoordinator` | Headless disconnect and retry produce exactly one run. |
| I05 | The backend serves live status: progress, counts, rate, loss and lifecycle. It survives client reconnect, and each live cell resolves to its exact event. | Coordinator, monitoring, E09 | Reconnect test; event references resolve. |
| I06 | Instant answers are F results bound to a revision. Deltas against a reference are computed in the backend. Repeat/Adjust/Continue creates a linked draft through the recipe authority. | F evaluator, I02 | Headless reference + run → deltas → linked draft. |
| I07 | Named metric definitions (population × parameter × statistic) are stored natively, and expressions use a validated backend grammar. F's statistics evaluate them. Quality flags (low n, missing calibration, loss, out of domain) come in the result. | Metric store, F statistics | Headless define → evaluate → flags present; an invalid expression is refused. |
| I08 | Criteria are stored as F populations (range, polygon, E interval) with a declared missing-value policy. Hit count and denominator come from F's evaluator. Live edits never deploy a sorting gate. | F population DAG | Headless criteria → hit count equals an independent oracle. |
| I09 | Multi-run comparison runs through F's ComparisonSpec. The backend gives compatibility and exclusion reasons. | F comparison | Headless compatible/incompatible run sets give the expected reasons. |
| I10 | Concentration-response fits come from F's fit authority, with unit, pairing, weighting, model and suspect-fit reasons. Insufficient input produces no fit. | F fit authority (F09) | Numeric fixtures, including the insufficient case. |
| I11 | QC records, versioned thresholds, verdicts and reversible exclusions are stored in the backend. Each verdict is computed against a threshold revision. Old runs are never re-evaluated silently. | Calibration records + QC store | Headless threshold change → new verdicts with lineage; old verdicts kept. |
| I12 | Experiments save, reopen, export and import with immutable sources. A finding links exact evidence and a new method draft. | `WorkspaceStore`, `NextRunPlanning` | Store/restart tests (exist on #815); finding → draft lineage. |

## F: FlowJo fold-in

**Track goal.** **One native analysis authority** evaluates populations,
statistics, comparisons and fits on **full-size MIB recordings and imported FCS
files**, with:
- exact event membership;
- persisted revisions;
- supervised, cancellable jobs.

Every host (Studio, Review, the I views) asks it for answers. No science runs in
TypeScript or in ad-hoc Python. The native C++ code is that authority. Cytoflow and
FlowKit are CI comparators only ([ADR 0016](../decisions/0016-native-analysis-authority.md)).

| ID | Backend goal (true when done) | Authority | Proof |
|---|---|---|---|
| F01 | The source library has stable source/sample IDs, a streaming fingerprint for any file size, verified relink, and acquired name vs label vs unit. | `AnalysisLibrary`, `ReviewSession` | A recording of more than 64 MiB fingerprints and relinks. |
| F02 | QC membership (good/bad/unknown) with reasons is persisted. Changing it invalidates dependents. | `SourceAnalysis` | Headless QC change → dependent results marked stale. |
| F03 | Transforms and compensation are persisted with order and scope and applied inside the evaluator. Unsupported channels are refused. | `ParameterTransforms` | Fixture values after transform and compensation. |
| F04 | The population DAG (interval, polygon, ellipse, Boolean, E interval) is evaluated with tri-state missing-value logic and exact membership at **full recording size**, with no 10,000-event cap. Draft/commit/undo and inherited/local revisions live in the backend. | `PopulationGraph` | The FlowJo 10.8.1 reference reproduces 229,585 / 190,705 through `PopulationGraph`. A real MIB recording of 200k+ events stays within a stated memory bound. |
| F05 | Plot data (bins, decimated points) is computed in the backend from exact membership, bound to a revision. | `PopulationPlot`/`PopulationScatter` | Binned counts sum to population n. |
| F06 | Membership → event references → EvidenceResolver. IDs are lossless (64-bit as strings), and expired images come with reasons. | `EvidenceGallery`, E09 | IDs above 2^53 round-trip; the expired case returns a reason. |
| F07 | Embedding jobs persist parameters, seed, input revisions and subsampling. Coordinates are keyed by event reference. | Worker job | Same inputs + seed + runtime give the same coordinates. |
| F08 | Statistics (n, mean, median, CV, percentiles) carry units and estimator definitions. Exact and histogram-derived values are labelled. Null denominators are explicit. | `PopulationStatistics` | Independent oracle per estimator. |
| F09 | Comparison and fits take a declared experimental unit, pairing, weighting and model. Incompatible sources and suspect fits are reported. | `AnalysisComparison` + fit authority | Numeric fixtures, including the insufficient-units case. |
| F10 | Report jobs have immutable inputs and deterministic ordering. Cancel and retry preserve prior artifacts. | `AnalysisReports` | Cancel/retry/name-collision tests. |
| F11 | Overlays are computed in the backend. Normalization never changes membership or statistics. | `AnalysisOverlay` | Statistics are identical with and without normalization. |
| F12 | The workspace persists with documented precedence. Reopen recovers definitions and revisions, and results are never reused after an incompatible change. | `WorkspaceStore` | Reopen after an incompatible change → recompute, not reuse. |
| F13 | Live analysis reads immutable watermark snapshots and never blocks the recorder. Final results bind to the finalized input. | `CommittedEventStore` | Stress test with the recorder unaffected; rig run. |
| F14 | Findings are immutable. A next-run draft is created only through I02, and analysis never actuates. | `AnalysisHost`, I02 | Headless finding → draft; no device command issued. |
| F15 | FCS 3.x import feeds the native evaluator. FCS and supported GatingML export declare units, transforms and unsupported constructs. | Import/export service | The FlowJo reference FCS imports and gates natively. |
| F16 | Jobs are revision-matched and survive worker crash, timeout, cancel and restart, with bounded memory and no stale cache acceptance. | `mib-analysis-worker` | Crash/timeout/cancel tests (exist); memory bound at full size. |
