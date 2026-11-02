# Analysis worker and native population core

Sources: `include/backend/analysis/`, `src/backend/analysis/`,
`crates/mib-analysis-worker/`, `workers/cytoflow/worker.py`.
Tracking: #808/#523. This is an initial F-A/F-B slice, not complete FlowJo support.

## Responsibility and boundaries

**Engine decision (2026-11-02, ADR 0016).** The native C++ code is the single
production analysis authority. Cytoflow 1.3.2 and FlowKit 1.3.2 are CI comparators
only and are not bundled; the worker keeps t-SNE (scikit-learn 1.9.1) as its one
production job. The Cytoflow paragraphs below describe the parity path.

The portable C++ population core is the authority for inclusive optional scalar
bounds, three-valued Boolean composition within parent/QC universes, exact keyed
membership validation and counts with null empty denominators. It never computes
E itself: values/validity must come from the existing LUT/source authority.
The Rust host calls this same C++ implementation through an internal cxx seam;
it has no independent scalar or Boolean evaluator.

The Python process uses real pinned Cytoflow 1.3.2 PolygonOp, linear coordinates
and exact opaque string row keys. It has no access to HDF5, images or hardware.
It rejects unsupported fields, expressions, duplicate keys, invalid geometry
and unbounded requests. Missing coordinates are explicit unknowns, never False
placeholders that become admitted by NOT.

## APIs

- C++ `evaluateInterval`, `logicalAnd/Or/Not`, `compose`, `countMembership`,
  `acceptWorkerMembership` in `Population.h`.
- Rust `Worker::run` validates pinned revisions/keys, launches a process group,
  applies byte/time/address-space limits, kills/reaps on failure/cancel, verifies
  complete worker key coverage, then composes an optional native scalar gate.
- `JobStore::begin/finish/query` keeps immutable checksummed request and terminal
  artifacts, no-replace publication, file/directory sync and an exclusive OS lock.
  Reusing a job ID with different input is refused. `recover_interrupted` records
  interrupted rather than replaying uncertain work. It fails before mutation when
  its directory scan budget is insufficient.
- Internal input `mib-analysis-batch/1` includes native scalar configuration;
  projected Python request `mib-cytoflow-polygon/1` excludes it. Neither is the public app schema. App interval analysis uses `review-population/1`
  (backend ABI 46 / Review ABI 8); the private worker protocol is unchanged.

## Threading, storage and limits

`Worker::run` is synchronous and must be scheduled off acquisition/UI threads.
It mutates no shared analysis state and communicates through bounded temporary
files, not pipes that can deadlock on a full output buffer. Unix process groups
and rlimits are implemented; Windows explicitly returns UnsupportedPlatform until
a qualified Job Object supervisor lands. No cross-platform execution claim.

Initial caps: 10,000 events, 128 polygon vertices (optional closing duplicate),
4 MiB request/output, caller deadline and memory cap. No million-event claim.
The job journal is private worker artifact recovery, **not** a second workspace,
recipe, method, finding or run-identity store. Source HDF5 is never modified.
Caller-supplied source fingerprints/revisions must come from the source adapter;
validating their transport does not independently verify a recording.

## Evidence / remaining work

[Module instructions](../../tools/flowjo_acceptance/README.md) include the synthetic
real-engine/native/journal demo. Native tests cover full Boolean truth tables,
scalar edges, invalid bounds, universes, exact keys and stale revisions. Python
tests exercise real engine geometry/identity plus refusals. Rust tests cover
terminal recovery, collision/corruption, cancellation, timeout, crash and budgets;
the separately enabled real-engine test covers C++ equality/unknown composition
and durable reopen. Native ASan/UBSan and Rust clippy pass locally.

The native interval/statistics slice is mounted in Studio/Review; the Cytoflow
worker is not yet attached to app source submission. No live provider, qualified source reader adapter,
workspace DAG/undo/redo, images, qualified advanced science or reports. Keep
[[ReviewSession]] and acquisition exclusion intact. Cytoflow distribution no longer
gates shipping (ADR 0016: comparator only, not bundled).

## Bounded native population graph

`PopulationGraph.h` adds plan ID/revision, stable node IDs, explicit parent,
predicate-mask reference or AND/OR/NOT operands. Validation includes parent and
operand edges in the same acyclic dependency graph. Execution is independent of
input node ordering, joins every mask by exact key/revision, rejects missing or
unused predicate inputs, and clips every result to parent and QC. Limits: 256
nodes; the keyed (string-key) path is bounded at 1,000,000 events and 16,777,216
node-events (F-1; was 10,000 / 1,000,000); excess is an error, never sampling.
Full recordings use the columnar evaluator below.
Empty populations retain revision identity. This is an internal scientific
contract, not a bridge ABI, serialized workspace document or a gate editor.

`analysis.graph` proves parent/operand cycles, invalid refs/arity, stale revisions,
unknown-safe NOT/QC, exact reordering, empty inputs and deduplicated unions. Both
native tests pass under ASan/UBSan. Overrides, undo/redo and native source adapters
remain separate implementation obligations.

## Exact population reductions

`PopulationStatistics.h` adds bounded (keyed-row bound, see above) revision-bound feature/unit
columns joined by opaque exact keys. It reports selected/excluded/unknown
membership separately from finite measured/missing selected values, and raw-value
minimum/maximum/mean/median (null when no finite selected measurement exists).
Nonfinite values count as missing; excluded and unknown events never contribute.
Full column coverage is mandatory even for excluded rows. Median uses the middle
value or midpoint of the two middle values; mean uses scaled compensated summation.
These are floating-point raw-data reductions, not histogram estimates or arbitrary
precision arithmetic. Feature conversion and mechanics computation are not performed.

`analysis.statistics` covers reordered >2^53 keys, conservation, stale/foreign/
duplicate/missing-row refusal, units, empty/singleton populations, finite extremes,
subnormal medians and size limits. This is not source completeness certification,
a table UI/export, or F08 acceptance. Existing Review quick-look stays unchanged.

## Production app interval slice

`ReviewSession::analyzePopulation` atomically reduces both stored metadata sets
through Population/PopulationStatistics. `ReviewJobs::analyzePopulation` holds the
existing job-lane fence; Studio's `BackendFacade::analyzePopulationJson` additionally
holds the existing idle-configuration transaction and refuses raw recording.
No second reader, HDF5 writes or acquisition coexistence. Caps and missingness
limitations are documented in `review-contract.json` and the actual UI.

One `PopulationAnalysis` React component mounts in Studio App and YOFO Review
ReviewPanel, via the respective production transports. Native Rust bridge parity
tests exercise actual HDF5 fixture reads; UI tests mock transport only. App
exposure does not imply polygon/DAG/workspace/live/report integration.

## Production graph and selected-image page (2026-10-23)

The existing `review-population/1` command now accepts an optional bounded graph:
interval predicates plus AND/OR/NOT, parent restrictions and an output node.
It calls the existing native PopulationGraph, not a renderer evaluator.
The selected top-level feature remains the reduction column. The backend rejects
cycles, unknown outputs and invalid fields/bounds before publishing results.

Optional `galleryOffset` copies up to 12 selected 64×64 grayscale thumbnails
under the same session lock as membership. Set-qualified decimal ordinals remain
attached; unavailable images are null and never remove scientific observations.
Each page is explicitly a fresh evaluation, not a durable snapshot lease.

Studio and Review share the graph JSON editor, browser-local recipe save/restore
and exact selected gallery. Browser storage errors surface; this is not a
source-fingerprinted portable workspace, revision history or relink mechanism.
Worker job scheduling and live-source integration remain unimplemented.

## Production application integration (2026-10-23)

[Production API](../../docs/design/flowjo-analysis/production-api.md) defines ABI47/
Review9 source-bound strategies, exact I lookup/evaluation, E source fencing and
pinned galleries. Strategy persistence reuses the metadata SQLite authority;
Cytoflow immutable job inputs/results use the existing single-owner job journal.
`mib-analysis-worker::app::Service` is shared by Studio and Review; its scheduler
owns and joins a bounded process worker, never a recording or acquisition handle.
Live/archive images remain explicitly unavailable without a provider locator.

Native statistics include documented type-7 quantiles, sample SD, CV and geometric
mean. AnalysisHost exports deterministic revision-bound population reports/CSV and
checksummed strategy manifests; import reuses save/CAS and verifies current source
bytes. Retained normalized source projections use the same metadata owner with a
900,000-byte cap, allowing explicitly historical evaluation after a live producer
advances. Retention does not fabricate image assets or scientific compatibility.

`PopulationPlot.cpp` owns bounded raw histogram binning/normalization. Saved plot
layers refer to native graph populations; renderer offsets never affect science.
Optional strategy QC stores exact-key attributed manual assertions; good-only
eligibility remains tri-state and the worker revision pins its native fingerprint.
No processing-validity-to-scientific-QC conversion is performed. Legacy optional
fields remain omitted so pre-extension command receipts keep their identity.

## Full-recording columnar evaluation (F-1, 2026-11-02)

Sources: `include/backend/analysis/PopulationGates.h`, `ColumnarEvaluation.h`,
`include/backend/review/RecordingColumns.h` (+ `src/`), tests
`tests/analysis/columnar_test.cpp` (`analysis.columnar`) and
`tests/review/review_population_scale_test.cpp` (`review.population_scale`).

- **One graph compiler.** `PopulationGates::compileGraph` (moved out of
  `SourceAnalysis.cpp`) validates interval/polygon/ellipse/quadrant/Boolean nodes;
  `gateTruth` decides one event (missing coordinate → Unknown).
- **Chunked columns.** `EventColumns::scan` delivers requested columns in
  bounded chunks in event order (segments, e.g. valid then invalid; NaN =
  missing). `RecordingColumns` adapts `Hdf5Service::readMetadataColumns` (members
  only, 65,536 rows per read; absent compound members → missing, never 0;
  `youngsModulus <= 0` → missing); `VectorEventColumns` holds in-memory columns.
- **Evaluation.** `evaluateColumns` runs the DAG per chunk with
  `logicalAnd/Or/Not` exactly as `evaluatePlan` (node = parent AND eligibility AND
  predicate) and stores each population in `TriStateBits` (two bits per event,
  rank lookup by popcount). The `MembershipSet` carries the `RevisionKey` (source
  identity + graph revision), chunk evidence and byte size.
- **Results.** `columnStatistics` (exact: every selected finite value through the
  shared `reduceMeasuredValues`), `populationCounts`, `columnHistogram` (exact
  bins), `columnScatter` (deterministic stride decimation), `eventReference`.
- **Bounds** (`ResourceBounds`): chunk 65,536; ≤2^32−1 events; bitsets ≤256 MiB;
  ≤2^26 exact-statistics values. Refusals name the bound.
- **Hosts.** `evaluateSource` (F-source/1) converts JSON rows to in-memory columns
  and uses the same evaluator (JSON rows bounded at `kMaxJsonSourceEvents` =
  100,000). `ReviewSession::analyzePopulation` uses `RecordingColumns` for the
  whole file (see [[ReviewSession]]).


## Workspace path at full scale: columnar source handles (F-1b, 2026-11-02)

Sources: `include/backend/analysis/SourceAnalysis.h`,
`src/backend/analysis/SourceCore.{h,cpp}` (internal), `PopulationPlot.cpp`,
`PopulationScatter.cpp`, `AnalysisHost.{h,cpp}`; test
`tests/review/review_workspace_scale_test.cpp` (`review.workspace_scale`).

- **Two source kinds, one core.** F-source/1 keeps inline JSON rows (≤100,000;
  live/run-archive evidence and retained live snapshots). **F-source/2** is a
  columnar *handle*: descriptor only (`storage: columnar`, sourceKey/fingerprint,
  scope, path, parameters + units, `segments` [{set, count}], `events`,
  `retention`), no rows. Event keys are `JSON[sourceKey, set, ordinal]`, the same
  strings the stored F-source/1 rows always carried, so saved bindings, QC keys
  and gallery keys are unchanged.
- **`ColumnResolver`.** `analysisRequest(db, request, write, source, method,
  columns)`: the host lends a handle's raw columns for the duration of a call
  (`ReviewJobs::withRecordingColumns` for stored files, `lendFcsColumns` for FCS).
  `SourceCore::withSource` builds a `SourceView` (raw columns + key/index
  functions) for either kind; row sources use `RowColumns` over the JSON rows in
  place (no copy).
- **Shared evaluation.** `prepare` applies ordered transforms per chunk
  (`TransformedColumns`, the former long-double arithmetic); `runGraph` validates
  gates/QC/revision and evaluates every node once (`evaluateColumns`);
  `evaluation` reduces one population (`columnStatistics`). `evaluateSource`,
  `evaluatePopulations` (reports: one membership pass for ≤32 populations),
  `selectedEventKeys` (gallery: rank lookup in the bitset), `plotSource`
  (`columnHistogram` per layer) and `scatterSource` (SHA-256 ranks kept in a
  `maxPoints` heap: the same sample as before, never every eligible point) all
  run on it. Row sources still return per-row `rows`; columnar sources return
  `membershipStorage: native-bitset` and `events` instead.
- **Every-row consumers.** `observations` (≤65,536 events, 4 MiB) and `jobInput`
  (worker jobs, ≤100,000) materialize F-source/1 rows from the lent columns
  (`materializeColumnarSource`) and refuse above, naming the bound.
- **Retained handles.** `retainSource` of a stored file stores the small
  descriptor (file-pinned); comparisons, overlays, libraries and report batches
  over retained handles re-read the file and re-verify its full-file SHA-256 on
  every evaluation; a changed file is refused, never reused.
- **Proof** (`review.workspace_scale`): row/columnar parity (evaluation with
  transforms + good-only QC, histogram, seeded scatter identical) on a 2,200-event
  file; on 255,000- and 260,000-event files a saved interval+polygon+Boolean
  strategy, its 4-population report, 48-bin plot and deep gallery equal an
  independent oracle; the strategy applied to file B, both retained, and a
  descriptive comparison (file A not open: short-lived reader) equals the oracle;
  open RSS < 24 MiB and the whole sequence < 64 MiB peak growth (asserted;
  measured values in the ledger).

## FCS 3.0/3.1 import feeding the native evaluator (F-2, F15 read side)

Sources: `include/backend/analysis/FcsReader.h`, `src/backend/analysis/FcsReader.cpp`;
tests `tests/analysis/fcs_reader_test.cpp` (`analysis.fcs_reader`),
`tests/analysis/flowjo_reference_test.cpp` (`analysis.flowjo_reference`), writer
`tests/analysis/fcs_test_support.h`; optional comparator
`tools/flowjo_acceptance/fcs_comparator.py` (never production, ADR 0016).

- **Reader** (native C++, no Python): HEADER offsets, `$BEGINDATA/$ENDDATA` when
  the HEADER records 0, supplemental TEXT; TEXT with any delimiter and doubled-
  delimiter escapes, case-insensitive keywords (`parseFcsText`); list mode with
  `$DATATYPE` F (32-bit), D (64-bit), I (8/16/32/64-bit per `$PnB`); `$BYTEORD`
  ascending or descending; integer `$PnR` masking (closed form, FCS 3.1: the
  smallest bit count holding `$PnR` − 1, `bit_ceil(ceil(R)) − 1`; `$PnR` 1 is a
  real mask of 0, `$PnR` 2 masks one bit; no mask when `$PnR` > 2^(bits−1), so a
  64-bit `$PnR` between 2^63 and 2^64 no longer overflows into an endless loop;
  `FcsParameter::mask` is `std::optional`); integer `$PnE` log decoding
  (`f2 * 10^(f1 * x / $PnR)`, f2 = 0 read as 1 and reported); `$PnG` division only
  with `FcsOptions.applyGain` (off by default; recorded either way).
- **Refused** (`FcsError`, nothing read): FCS2.0, `$MODE` C/U, `$DATATYPE` A,
  packed integer widths, mixed byte orders, F/D width mismatch, DATA outside the
  file or shorter than `$TOT` × width, missing/duplicate keywords, malformed TEXT.
- **Reported, not applied** (`unsupported`, in the handle's `import` block):
  compensation (`$SPILLOVER`/`$SPILL`/`$COMP`), `$PnCALIBRATION`, `$PnD`,
  `$TIMESTEP`, `$PnE` on float data, `$NEXTDATA` data sets, the ANALYSIS segment,
  trailing DATA bytes, unapplied `$PnG`.
- **Columns.** `FcsColumns` is an `EventColumns` (one segment `events`): each
  scan reopens the file and reads at most min(chunk, 4 MiB / event width) events
  per read. Parameter ids are `$PnN` made graph-safe and unique; units are
  explicit (`a.u. (FCS channel value)`, `... log-decoded scale value`,
  `... channel value / $PnG`).
- **Handle.** `fcsSourceDescriptor({kind: fcs, path[, applyGain]})` is the
  F-source/2 handle (scope `fcs`, full-file SHA-256 identity; with applied gain a
  distinct fingerprint); `lendFcsColumns` checks the file's size and write time
  against those its full-file SHA-256 was computed for (the hash is recomputed when
  they differ and must equal the descriptor's), lends `FcsColumns`, then re-stats
  after the scan (post-scan fence, as `withRecordingColumns`): a change during the
  scan is refused. A rewrite keeping both size and write time is not detected (no
  full re-hash per evaluation); the descriptor's `retention` text says exactly this.
- **Parameters.** Up to 1,024 (`kMaxColumnarParameters`, equal to the reader's
  `$PAR` bound; formerly 64 in `SourceCore`, so wider panels imported but never
  evaluated). Columns stay lazy: a scan reads only the referenced parameters.
  Materialized F-source/1 rows are bounded by cells (`kMaxJsonSourceCells`,
  100,000 × 64). Both hosts resolve `kind: fcs` and
  lend `scope: fcs`, so saved strategies, reports, plots, comparisons run on FCS
  files through the same evaluator as MIB recordings.
- **Histogram bins.** `ResourceBounds::maxHistogramBins` (default 256, the display
  hosts' bound) lets the acceptance compare FlowJo's 4,096-bin exports.

## Comparison and concentration-response fits (F-4, F09 / I09 / I10)

The native inference authority (ADR 0016; no TypeScript or Python in the path):

- `include/backend/analysis/AnalysisInference.h` / `src/backend/analysis/AnalysisInference.cpp`:
  pure C++ distributions (regularized incomplete beta and gamma, normal, t
  quantile by bisection) and tests on unit-level values: Welch t, Mann-Whitney U
  (exact null distribution when untied and both n <= 50, else normal
  approximation with tie and 0.5 continuity correction), paired t, Wilcoxon
  signed-rank (zeros dropped; exact when untied and n <= 50, else normal
  approximation with tie correction), one-way ANOVA, Kruskal-Wallis; and
  `fitLogistic`: 4PL / 3PL (hill 1) by Levenberg-Marquardt (three starts,
  bounds |hill| <= 10, logEC50 within 3 decades of the tested range), covariance
  s^2 (J'WJ)^-1, t CIs, EC50 CI on log10 or linear scale, residuals, R^2,
  optional 1/variance-of-replicate-units weights.
- `compareSpec` / `fitSpec` (`AnalysisComparison.cpp`): parse
  `F-comparison-spec/1` and `F-fit-spec/1`, evaluate each pinned sample through
  the host (`evaluate` with the optional `metric {population, parameter}`, which
  adds `sourceParameters` / `sourceProvenance`), exclude incompatible sources
  with reasons (parameters/units, preprocessing fingerprint, processing core,
  calibration, E-LUT, missing metric, repeated source), average technical
  replicates per unit, and run inference only when declared and every group has
  >= 3 units (`kMinimumUnitsPerGroup`; can be raised, not lowered), otherwise
  `insufficient` with reasons and no p-value. Fits with fewer points than
  parameters + 1, fewer distinct concentrations than parameters, no response
  variation, or unweighable replicates return `insufficient` and no parameters;
  suspect reasons: stalled (damping > 1e16, `termination: stalled`) or not
  converged, EC50 outside the tested range, EC50 CI wider than the range (judged
  on the log10 interval in both reporting scales), linear-scale EC50 CI reaching
  <= 0, fewer than 5 concentrations, inverse-variance weights from fewer than 3
  replicates per concentration, parameter at bound, singular covariance.
- Each test reports `status` `computed` or `undefined` (zero within-group or
  paired-difference variance, all values tied, every paired difference zero, or a
  non-finite intermediate) with `undefinedReason` and null statistic / p-value /
  CI; never p = 0 or 1 for a degenerate input. Confidence levels follow one rule,
  `checkConfidenceLevel` (0.5, 0.999].
- Host actions `comparisonSpec` / `concentrationFit` (read endpoint; reply
  carries `contentHash`). The descriptive `compare` (F-analysis-comparison/1)
  stays for existing findings.
- Proof: `analysis.inference` (scipy values from
  `tools/analysis_inference/generate_reference.py` → `tests/analysis/inference_reference.h`)
  and `analysis.comparison_spec` (the same numbers through `AnalysisHost`, plus
  exclusions, insufficient units, technical replicates, suspect and insufficient
  fits).
- Remaining: no CI for rank-test shifts or ANOVA effect sizes; repeated-measures
  (paired > 2 groups) and dose series within one unit are refused, not modelled;
  no multiplicity correction; no I-6 view yet.
