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
