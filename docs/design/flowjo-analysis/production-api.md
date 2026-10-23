# F production analysis API

## Provenance and authority

F imports the required native E producer/recording changes from
`a26d26bac596a229cc2116f8666aa73de45b7ed1`, and the I metadata owner/store from
`5e9ec3c5d63e24bd9ede241db3c991b629010135`. No peer branch is merged or modified.
F adds a read-only `receipt` query to WorkspaceStore/SqliteService so an exact
save retry can return its original revision before re-resolving a changed source.
The receipt compares the complete canonical document, expected revision and operation.

Studio ABI 47: `analysis_read(request)`, `analysis_write(request)`,
`fetch_live_evidence()`. Review ABI 9: `review_analysis_read(request)` and
`review_analysis_write(request)`. JSON request object under `args.request` over
WebSocket/Tauri. Writes and analysis job commands require Studio controller status.
Native facade: `BackendFacade::analysisRequestJson(json, write)`.
Review uses the same host and source evaluator without creating hardware services.

Strategy revisions live in **app.sqlite3**, using the same metadata authority and
`workspace_revisions`/`workspace_commands` tables as I. IDs are namespaced
`analysis/<planId>`. No browser recipe is treated as backend state. The UI journals
an exact unresolved command before submission and retries that same command.

## Exact plan lookup and evaluation for I

```json
{"action":"read","planId":"plan","revision":"1"}
```

Returns `status:"ok"`, `planId`, decimal-string `revision`, and typed `document`
(`schema_version:1`, `type:"F-analysis-strategy/1"`, `graph`, `feature`,
`sourceBinding`, `method`). Missing exact revisions return `not_found`.
Null/absent revision is invalid; there is no fallback to the moving head.

```json
{"action":"evaluate","planId":"plan","revision":"1","source":{"kind":"stored","path":"/absolute/recording.h5"}}
```

Returns `status:"ok", evaluation:{schema:"F-evaluation/1", planId, revision,
sourceFingerprint, sourceKey, graph, feature, unit, selected, excluded, unknown,
measured, missing, minimum, maximum, mean, median, rows, populationComplete,
denominator}`. Counts and ordinals are decimal strings. Empty reductions and
unknown denominator are null. `rows` contains exact keys and membership
`selected|excluded|unknown`. Source fingerprint/key/parameter mismatch returns
`source_mismatch`, never implicit reapplication. Evaluation does not apply a
method, change acquisition, or claim scientific compatibility.

```json
{"action":"method","method":{"method_id":"method-id","revision_id":"revision-id","content_hash":"exact-hash"}}
```

Studio resolves all three against the verified registry revision cache, returning
`status:"ok"`, exact `method`, `config_sha256`, `scientificCompatibility:"unknown"`,
`hardwareReadiness:"not_evaluated"`, `applied:false`. Missing cache is `unavailable`;
identity/hash disagreement is `reference_mismatch`. Review has no registry and
returns `unavailable`. Neither host fetches/applies a moving latest revision.

## Source snapshots, saves and history

Read `{"action":"snapshot","source":...}` returns `source` (`F-source/1`).
Source descriptors: `{kind:"stored",path}`, `{kind:"run-archive",path}`;
Studio also `{kind:"live-run"}` and `{kind:"live-epoch"}`.
Stored files must already be open through the normal Review path. Full-file SHA256
is fenced against opened size/write-time, capped at 64 MiB and 10,000 observations.
No pathname-only or partial fingerprint. Native E leases fence reads; E batch
watermark is exclusive and distinct from event ordinal. Live/run-archive projection
fingerprints exclude access scope/finalized flag and preserve exact rows/coverage.
A running source can advance between snapshot and save: save then refuses and
requires explicit refresh, never silently pins different rows.

Save on write endpoint:

```json
{"action":"save","planId":"plan","expectedRevision":null,"commandId":"unique-command","graph":{"id":"plan","revision":"draft1","output":"gate","nodes":[{"id":"gate","parent":"","kind":"interval","feature":"area","minimum":100,"maximum":110}]},"feature":"area","source":{"kind":"stored","path":"/absolute/recording.h5"},"sourceBinding":{"fingerprint":"sha256:...","sourceKey":"sha256:...","parameters":{"area":"pixel2","deformability":"dimensionless","youngsModulus":"kPa"},"scope":"stored"}}
```

Copy all four binding members from the native snapshot. Null expectedRevision is
create-only; string is exact compare-and-swap. Optional `method` is the exact
three-part reference above, resolved before a new write. Exact successful retries
return the original receipt after head/source changes or restart. Collisions are
invalid. SQL transaction rollback prevents a revision without its receipt.
`history` accepts planId, optional after revision/limit (1..1000).
`restore` writes a new revision with planId, expectedRevision, commandId,
targetRevision; original revisions remain immutable.

## Pinned gallery

Read action `gallery` takes the evaluate fields plus unsigned `offset` (0..10000).
It returns up to 12 selected exact keys, source fingerprint, plan/revision and
pixels (64×64 grayscale bytes) or null. Stored images are read under the existing
Review lane/session locks after another exact fingerprint check. Source changes
refuse the whole request. E live/archive rows explicitly return unavailable images:
event ordinals are **not** saved image indices and E supplies no retained locator.

## Cytoflow application jobs

Transport `analysis_job` (Review `review_analysis_job`) accepts:

```json
{"action":"submit","submission":{"commandId":"unique-job-command","planId":"plan","revision":"1","source":{"kind":"stored","path":"/absolute/recording.h5"},"sourceFingerprint":"sha256:...","x":"area","y":"deformability","vertices":[[0,0],[1000,0],[1000,1],[0,1]]}}
```

The native `jobInput` read freezes source and evaluates the exact persisted graph
in one request. Rust maps keyed graph eligibility to a native scalar predicate
(1/0/unknown), intersected with the real Cytoflow polygon using native tri-state
composition. It does not accept browser-supplied scientific rows. Revision keys
pin source, plan/revision, parameters/units, QC policy and engine.

One active job; 10k events, 4 MiB journal/process output, 60 s deadline, 4 GiB address
space limit. No worker owns a bridge/recording handle. Shutdown cancels/kills/reaps
and joins. Environment configuration `MIB_CYTOFLOW_PYTHON` and
`MIB_CYTOFLOW_WORKER` is host-owned, never accepted from a browser.
Unconfigured platforms/runtimes fail explicitly. Unix is qualified; Windows
supervision/distribution remains open. Existing journal stores immutable inputs
and terminal results under `<data>/analysis-jobs`; reopening marks pending jobs
Interrupted and never silently reruns them.

`query`/`cancel` take jobId; `reconcile` takes original commandId (lost reply);
`status` reports recovered IDs/runtime configuration. Terminal output includes
its original source/plan identity even if the current UI source differs.
A failed terminal publication is reported as durability_failed, not success.

## Evidence and limits

- Native producer → HDF archive → actual Review reader → F adapter: fingerprints,
  keyed masks and reductions equal; processing errors preserve unknown denominator.
- Strategy host: CAS race, receipt collision/replay, source mismatch, fault rollback,
  reopening; Review bridge: source-bound save/reopen/evaluate/gallery key parity.
- Scheduler: real Cytoflow, graph unknown propagation, single active job,
  cancellation/join, durable reopen; UI: journal-before-write, storage failure,
  lost-reply reconciliation.
- Hardware EGrabber acquisition and Windows installer/runtime are not qualified by
  mock/synthetic QA. TSan cannot start on this host (`unexpected memory mapping`).
- Full F01–F16 acceptance remains tracked separately; these APIs do not supply
  compensation, embeddings, scientific QC qualification or report composition.

## Retained sources, tables and portable strategies

`analysis_write({action:"retainSource",source,commandId})` freezes the native source once,
then stores its complete normalized immutable projection in the shared metadata DB
under `analysis-source/<commandId>`, revision 1, atomically with its full request
and checksum. Exact command retries return the original snapshot after source
advancement/restart; changed commands collide. Returns source, contentHash and
descriptor `{kind:"retained",id:"original-command-id"}`. Use that descriptor in subsequent saves,
evaluation, gallery, report or jobs; it does not retarget when the live producer
advances and survives application restart. The projection cap is 900,000 bytes
(no sampling); this is separate from event count and the SQL 1 MiB document cap.
The returned fingerprint remains the scientific source fingerprint, not the
storage-envelope ID. Retention does not create E image assets. Stored gallery
still checks its recorded pathname against the currently open reader and bytes.

Evaluation additionally returns type-7 p05/p25/p75/p95, sample SD (n−1),
CV percent (`100*sampleSD/abs(mean)`), and geometric mean
(`exp(mean(log(x)))`, requiring all measured values positive). Singleton SD/CV,
zero-mean CV, empty reductions and nonrepresentable results are null. These are
raw finite-value reductions, never display/histogram estimates.

Read action `report` takes the exact evaluation fields. For up to 32 saved graph
nodes it produces a reusable population table using the same native evaluator,
full saved strategy/source binding, explicit estimators/QC/compatibility, and a
deterministic report contentHash. Returned CSV has explicit headers and empty
cells for unavailable values; user-authored label cells are quoted and protected
against spreadsheet formula interpretation. JSON is the complete evidence export;
CSV is its tabular projection, not a standalone provenance package. This is not a
full report-layout designer or a multi-run inferential comparison.

Read `manifest` takes planId/revision and returns `{manifest,contentHash}`.
Manifest schema F-analysis-manifest/1 contains the exact typed document and origin
revision. Write `import` takes those two members, source, expectedRevision and
commandId. It checks the content hash and reuses the normal verified save/CAS/
receipt path, preserving the plan ID and complete source-bound document. It cannot
silently import against different bytes. A recording copied to another path can
be opened normally and matched by full-file hash. Content hashes detect accidental
changes; they are not signatures or authentication. Import never applies methods.

## Saved plots and explicit QC assertions

An optional `plot` on save/import is persisted with the strategy:

```json
{"kind":"histogram","feature":"area","minimum":0,"maximum":200,"bins":40,"normalization":"count","layers":[{"population":"area-gate","displayOffset":0}]}
```

Read action `plot` takes exact evaluation fields. Native F-plot/1 supplies edges,
exact count strings, normalized numeric values, source/plan/revision and a content
hash. Limits: 1..256 distinct representable bins, 1..8 distinct graph populations,
finite offsets bounded to ±1,000,000. Bins are [left,right), final bin closed;
underflow/overflow and missing/unknown counts are explicit. `fraction` divides by
all finite selected measurements, including outside the display range; `density`
further divides by bin width. Empty normalized layers are null, not invented zeros.
Offsets are presentation-only and cannot change counts, gates or reductions.
The production renderer consumes native values and exports a self-contained SVG.

An optional `qc` on save/import contains `policy` (`all` or `good-only`) and
`decisions`: `{key,status,actor,reason}`. Status is good/bad/unknown; foreign or
duplicate event keys, absent author/reason and unbounded strings are refused.
Unlabelled events remain unknown. `good-only` composes native three-valued QC with
the graph; `all` does not filter but still reports distinct QC counts. Processing
valid/invalid status never supplies scientific QC. These are attributed manual
assertions, **not independently qualified automated QC**. They are immutable in
each saved revision and survive manifest round-trip. Jobs pin the native QC
fingerprint and use the QC-aware graph eligibility, preserving unknown values.
The pinned gallery can stage attributed labels; saving a new revision is required
before they affect evaluation.

Optional plot/QC fields are omitted, not inserted as null, for legacy save
commands so existing command receipts still replay identically.

## Template applications and experimental-unit findings

Write `apply` accepts `planId`, `expectedRevision`, `commandId`, `source`,
`sourceBinding`, and `application: {planId, revision, overrides}`. The upstream
revision is exact; each override is `{node, minimum, maximum}` for an existing
interval. Missing/changed nodes require explicit migration. The derived graph is
saved through the same CAS/receipt authority. Reapply with a new upstream revision
to resynchronize; history never moves. Plot/method references are inherited;
manual event QC decisions are cleared. Applied-manifest import requires its exact
upstream strategy to exist first. This is analysis application, not hardware apply.

Read `compare` accepts `spec` with exactly:

```json
{"conditions":["before","after"],"design":"independent",
 "weighting":"equal-experimental-unit","descriptiveOnly":true,
 "endpoint":{"feature":"area","statistic":"mean","unit":"pixel2"},
 "samples":[
  {"condition":"before","experimentalUnit":"a","planId":"plan-a","revision":"1","source":{"kind":"retained","id":"snapshot-a"}},
  {"condition":"after","experimentalUnit":"b","planId":"plan-b","revision":"1","source":{"kind":"retained","id":"snapshot-b"}}]}
```

Two conditions, 2–32 retained samples; endpoint mean/median/p05/p95. Paired design
requires matching experimental-unit labels. Independent design refuses shared unit
labels. Both refuse repeated source fingerprints or source identities: different
windows of one acquisition do not become independent replicates. Each unit
contributes one native endpoint with equal weight, never individual cell rows as
replicates. Any missing endpoint makes its aggregate unavailable. Difference is
right minus left. Compatibility stays unknown; no p-value, CI, fit or causal claim.

Write `saveFinding` takes `findingId`, `commandId`, `spec`, `note`; creates immutable
revision `1` in the same metadata authority. Read `readFinding` takes `findingId`
and exact `revision:"1"`. Replies include typed document, pinned samples, comparison
and canonical content hash. Same-request replay returns the original; identity
reuse with different content refuses. I may link this exact finding ID/revision/hash
without changing method apply authority. Next-run draft creation is not implicit.

## Saved report layouts and immutable runs

`reportSave` (write): `reportId`, `expectedRevision`, `commandId`, `layout`.
Layout is `{title,blocks}`; 1–8 unique blocks `{id,title,kind,reference}`. Kind table
or plot requires `{planId,revision,source:{kind:"retained",id}}`; finding requires
`{findingId,revision:"1"}`. Only exact revisions, no moving source/head. Report IDs
are 1–128 ASCII letters/digits/`-_.`. Layout revisions use shared CAS and receipts.
`reportRead` (read) takes `reportId,revision`.

`reportBuild` (write) takes `reportId,revision,runId,commandId`; evaluates all blocks
and atomically stores one immutable `F-report-run/1` in the existing DB. A failed
block publishes nothing. Run IDs cannot overwrite/rebind old runs. Same-request
retry returns original content. Artifact hash pins every rendered block and layout.
Maximum document 900,000 bytes; budget failure is explicit, never truncation.
`reportRunRead` (read) takes `reportId:runId,revision:"1"`; checksum verifies stored
artifact without opening sources or reevaluating newer strategies. Production UI
renders tables/plots/findings and exports evidence JSON or standalone escaped HTML.
This is bounded ordered-block composition, not arbitrary drag/drop or background
multi-report batch scheduling. Files are explicit downloads, not silent overwrites.

## Ordered derived parameters

Optional strategy `transforms` is an ordered array of up to eight stages, each
`{input,inputUnit,output,kind,scale,offset}`. Kind linear/asinh/log10 first computes
`(input-offset)/scale`, then the named function (linear is identity). Scale is
finite and positive, offset finite; input unit must exactly match the raw or prior
stage unit. Output is a new named dimensionless parameter, never overwrites raw.
Missing/nonfinite inputs, log domain failure and overflow produce null. No clipping,
compensation, inferred calibration or Logicle support. Native graphs, reductions,
plots and job inputs share this same projection. Parameter fingerprints include
raw units and ordered stages; real worker revisions pin the fingerprint. Comparisons
refuse different preprocessing identities even when displayed units match.
Applications inherit exact template transforms; imports/restore preserve them.

## Seeded embedding job artifact

Application job submission optionally includes
`embedding:{engine:"sklearn-tsne-1.9.1",seed:42,perplexity:5,maxIter:500,maxPoints:1000}`.
The same existing cancellable/durable job runs Cytoflow polygon AND native graph
eligibility, then embeds only selected finite x/y events. Two explicit parameters
(raw or derived); no implicit scaling. F07 is not a claim of high-dimensional
feature-selection UI or UMAP support. Seed is uint32, iterations 500–2000, maxPoints
3–1000, perplexity positive and strictly below actual sampled count. Insufficient
samples refuse; they do not change perplexity silently.

Sampling is explicit: sort eligible exact keys by hexadecimal SHA256 of UTF-8
`decimalSeed|key`, then key as tie-break; take maxPoints. Rust independently verifies
sample identity/order, count, eligibility, seed/config and finite coordinates.
Artifact stores eligible count, exact-key points and full spec in the immutable
terminal journal. Unselected/unknown events remain in the original membership map.
Fixed algorithm: sklearn 1.9.1 TSNE, 2D, random initialization, exact method, Euclidean
metric, early exaggeration12, learning rate200, no-progress300, min-gradient1e-7,
one job/thread. See [upstream TSNE documentation](https://scikit-learn.org/stable/modules/generated/sklearn.manifold.TSNE.html).
Repeated same-runtime seeded runs and reordered-input parity are tested; cross-BLAS,
cross-version and cross-platform bitwise equivalence are not claimed. Coordinates
are exploratory, not cluster/QC labels or effect evidence. UI displays keyed points
and downloads the pinned terminal artifact. Lossless JSON float parsing is enabled
for checksummed job persistence; no decimal round-trip drift is tolerated.

## Sample library annotations and display preferences

`librarySave` (write) takes `libraryId,expectedRevision,commandId,library`;
`libraryRead` (read) takes `libraryId,revision` (exact). Shared SQLite CAS/receipts.
Library: `{title,groups,preferences,samples}`; at most 128 samples and 64 declared
unique groups. Sample: `{source:{kind:"retained",id},label,groups,tags,notes,preferences}`.
Native resolution attaches source fingerprint/key/units/scope; clients cannot write
those bindings. Duplicate fingerprints, undeclared groups and duplicate tags refuse.
No recording or scientific unit is edited. Same-fingerprint relink is supported by
opening a byte-identical source and retaining its new verified locator; replacing
the sample descriptor appends a library revision, not a new measurement identity.

Display preferences support decimalDigits2–8 and showUnknown boolean. Effective
precedence is sample > library > application defaults (4,true), with per-field
origin in the read reply. The UI explicitly applies a sample's display precision
to workspace reductions; exports retain full native values. Unknown scientific
counts remain visible. This does not implement arbitrary global user preferences.

## Shared metadata namespace authority

Generic `SqliteService::workspaceRequest` refuses writes to `analysis/` and
`analysis-` namespaces. Only native AnalysisHost/Reports/Library call
`analysisWorkspaceRequest`; no JSON capability flag grants access. This boundary
must be preserved when integrating I's generic workspace RPC, otherwise arbitrary
version1 workspace documents could spoof retained sources or findings. No public
ABI is added. Native tests attempt generic writes to strategy/source/finding/report
namespaces and require refusal; typed analysis writes still pass.

## Saved scatter specification

Strategy PlotSpec also accepts
`{kind:"scatter",x:"area",y:"deformability",seed:42,maxPoints:1000,layers:[{population:"gate"}]}`.
Up to8 unique populations, uint32 seed, display budget1–1000. Only finite selected
coordinate pairs are drawable. Native output reports selected/unknown/missing and
eligible layer-event counts, all-eligible coordinate bounds, exact-key points and
parameter/source fingerprints. Display points rank by SHA256 of canonical JSON
`[seed,population,eventKey]`, then population/key. Input order cannot change the
sample. An event may appear in multiple layers. Sampling never changes native gates,
statistics or job inputs. Empty scatter is valid and keeps null bounds. Production
renderer supports raw/derived scatter in both saved plots and immutable reports.

## Multi-source histogram overlays

`overlaySave` (write): `overlayId,expectedRevision,commandId,spec`; spec contains
`title,layers` (1–8), each `{id,label,population,displayOffset,reference}` with
reference `{planId,revision,source:{kind:"retained",id}}`. Every saved source plot
must be a native histogram with exactly matching edges, feature, units, preprocessing
fingerprint and normalization. Selected layer must exist. Each layer preserves its
native counts/values and denominator; the overlay's explicit offset replaces only
the display offset. Same source in distinct layers is allowed, not treated as a
replicate. Scientific compatibility stays unknown.

`overlayRead` and `overlayEvaluate` (read) take exact `overlayId,revision`.
Evaluation returns F-overlay/1 with source-set fingerprint, exact source/plot hashes,
original population references and unchanged native values. Save uses CAS and
receipt-before-reevaluation replay. Report layouts may include kind `overlay` with
reference `{overlayId,revision}`; immutable report runs freeze its entire artifact.

Embedding feature-selection extension: optional `embedding.features` names2–16
unique raw/derived parameters. Omitted/empty preserves historical job x/y behavior.
The native source projection supplies values; clients never submit event matrices.
The host validates selection before allocating rows, pins feature order in the
immutable request/spec and independently verifies all finite-feature eligibility.
Selected events missing any requested feature retain their original population
membership but are omitted from embedding with explicit `missing_features` count.
No imputation/implicit scaling. Older two-feature job requests/results reopen
unchanged; optional fields are omitted from canonical serialization when absent.

### Draft graph editing and content identity

The production editor supports interval/Boolean forms, exact-reference renaming,
reference-safe deletion and bounded draft undo/redo. Native validation remains the
publication authority. Editing the same plan preserves its CAS revision; changing
plan/source selection clears it. Loading an exact saved graph restores method,
transforms, QC, PlotSpec and application lineage. Detaching template lineage is an
explicit draft action; it never rewrites saved history. Plot population references
still require explicit repair after a rename and native validation refuses stale ones.

`evaluate` now includes `graphFingerprint`, SHA256 of the complete graph document.
Native graph revision keys use this content identity rather than trusting a reusable
user-authored draft revision label.
Selecting an arbitrary revision or restoring/applying a strategy does not imply its
document has been loaded into the editor. Save refuses until exact loading completes;
this prevents editor defaults from silently replacing unobserved provenance.

The graph form can add a rectangular population as named `id-x`, `id-y` inclusive
intervals and an `id` AND population. It sets the new population as output, refuses
name collisions/reversed bounds, and leaves both components editable. No separate
browser membership implementation or implicit coordinate transformation is used.

### Reusable population tables

Strategy save/import/application supports optional `table:{populations,columns}`.
Populations are 1–32 unique exact graph IDs in output order; columns are unique native
statistic fields in output order. `population`, `feature`, `unit` are mandatory.
Other supported columns: selected, excluded, unknown, measured, missing, minimum,
maximum, mean, median, p05, p25, p75, p95, sampleStandardDeviation, cvPercent,
geometricMean, denominator. Missing population/unknown column is an error, not an
empty fabricated row. Omitting table preserves the standard all-population table.

`report` returns explicit ordered `columns`; CSV and immutable report rendering
follow them. Values remain native reductions over all eligible rows. This is a
reusable projection, not a client-side formula language or new statistical estimator.

### Saved-record discovery

Read `catalog {kind, after?, limit?}` lists metadata-only head IDs/revisions for
strategy, library, source, report, finding or overlay. Limit1–100 (default50),
`after` is the last returned ID. `next:null` ends the page sequence. Literal namespace
ranges and a keyset cursor avoid wildcard interpretation/duplicate offset pages.
Pages are discovery, not a frozen inventory; opening/evaluating still requires an
exact revision and source. The internal database catalog path is unavailable through
the generic workspace endpoint. Libraries have exact-open buttons; strategy/source
catalogs expose references without silently replacing the current draft or source.

Saved scatter plots can stage a rectangular draft by dragging after loading the
exact edit revision and entering a unique rectangle ID. Screen coordinates map to
the artifact's explicit raw/derived bounds; reversed drags are normalized, tiny or
cancelled gestures produce nothing. The result is the same explicit interval/AND
composition as the numeric form. It cannot mutate an attached template application
without explicit draft detachment, and it never saves/evaluates automatically.
