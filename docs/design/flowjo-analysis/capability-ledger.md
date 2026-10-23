# FlowJo capability acceptance ledger

Status: native composition and bounded real-engine worker implemented; full integration incomplete. No F capability is fully accepted.

Backend evidence base: `0a58e201`; UI/design base: `71337496` (freshly fetched branch heads, not a reset). Paths under `tests/review/` unless otherwise shown. Test names below are inspected existing evidence, **not tests rerun in this audit**. Original board IDs and research hashes: [catalogue](evidence-catalogue.json). Each FJ maps to the corresponding #523 guide/research package; board coverage supplements rather than replaces FJ requirements.

| ID / research | Original boards | Existing code / PR / test | Disposition | Owned implementation package | Remaining acceptance |
|---|---|---|---|---|---|
| F01 / FJ01 | 13,15,16 | ReviewSession / ReviewTypes::RunIdentity; UI open/libraryModel.ts; review_run_identity_test.cpp; #746/#753 | extend | #523 A01/A02,FJ01; #808 coordination | Verified relink/fingerprints, labels/units, metadata editing and deduplicated groups absent. |
| F02 / FJ02 | 11 | RunStatistics descriptive flags; processing validity; review_run_statistics_test.cpp; #786 | missing | #523 A03,FJ02; #808 coordination | No persisted good/bad/unknown scientific QC graph; descriptive flags are not QC qualification. |
| F03 / FJ03 | 11 | SourceEModulusLut preserves mechanics provenance (not compensation); review_source_elut_test.cpp; #764/#783 | missing | #523 A03,FJ03; #808 coordination | Compensation controls, estimation/applicability and ordered transforms absent. |
| F04 / FJ04 | 01,12,16 | Processing target predicate and EModulusLut; review charts; review_charts_json_test.cpp | restore | #523 A02/A03/A04,FJ04; #808 coordination | Native tri-state Boolean/parent/QC composition and inclusive scalar predicates now tested; bounded population DAG now tested; persisted full plans, overrides/resync and reversible edits remain; sort target is not the analysis authority. |
| F05 / FJ05 | 10 | review/charts/ChartsView.tsx and chartData.ts; review/charts/charts.test.ts; #801 | extend | #523 FJ05; #808 coordination | Saved revision-bound gallery/PlotSpec and bounded scientific queries absent. |
| F06 / FJ06 | 02,17,18 | ReviewSession::fetchImage, ThumbnailGrid, inspectModel/packetCrop; review_inspect_test.cpp; reviewIdentity.test.ts; #757/#774 | extend | #523 A01,FJ06; #808 coordination | Existing record/image inspection; population membership, durable EventRef mapping and live captured asset retention absent. |
| F07 / FJ07 | none: FJ07 research | No production embedding adapter found; No acceptance test found | missing | #523 A03,FJ07; #808 coordination | Seeded embedding artifacts, exact event map, subsampling policy and repeatability absent. |
| F08 / FJ08 | 01 | RunStatistics.cpp exact descriptive run medians; review_run_statistics_test.cpp; #786 | extend | #523 A03,FJ08; #808 coordination | No reusable population tables, CV/percentile definitions or revision-bound table export; retain quick-look authority. |
| F09 / FJ09 | 03,13 | RunStatistics deltas; #314 comparison specification; review_run_statistics_test.cpp; #801 | restore | #523 S02,FJ09; #808 coordination + #314 | Endpoint compatibility/design/weighting/fits missing; R5 unscheduled in old plan. |
| F10 / FJ10 | 09 | ExportEngine / ReviewJobs batch export; review_export_engine_test.cpp; #802 | missing | #523 FJ10; #808 coordination | Existing batch file export is not saved report composition/batching. |
| F11 / FJ11 | 01,10 | ReviewCharts histogram display; review_charts_json_test.cpp; review/charts/charts.test.ts | extend | #523 FJ11; #808 coordination | Saved multi-layer OverlaySpec/normalization/offsets with independent numeric semantics absent. |
| F12 / FJ12 | 11,16 | Existing review UI state and settings; source files read-only; review_session_test.cpp | missing | #523 A02,FJ12; #808 coordination | No portable analysis manifest, revisioned undo/redo, recovery/relink or preference precedence implementation. |
| F13 | 05,06,17,18 | MonitoringCellStore for retained images, not unbiased event history; monitoring cell tests; #749 | missing | #523 L01; #808 coordination | No immutable unbiased metrics snapshots/watermarks; HDF5 reader/writer exclusion blocks naive reuse. |
| F14 | 04,07,08,14,19,20 | RunStartIdentity/method provenance; #314 research loop; review_run_identity_test.cpp | restore | #523 S01/S02/S03,E01; #808 coordination + #314 | Pinned references/findings/next-run evidence linkage absent; existing methods retain apply authority. |
| F15 | FJ interchange research | FcsWriter.cpp + HdfExportService; tests/recording/fcs_writer_test.cpp; scripts/test_fcs_flowio.py; #520 | extend | #523 I01 / #553; #808 coordination | Native per-detection export exists; FCS import, GatingML, React export and qualified per-cell policy missing. |
| F16 | 15 | ReviewJobs export/processing lanes; safe Reanalysis publication; review_jobs_lanes_test.cpp; review_reanalysis_test.cpp; #742 | extend | #523 A01/A02/A03; #808 coordination | Unix Cytoflow process supervisor, revision/key acceptance and atomic private terminal journal implemented and tested. Windows supervision, workspace integration, scheduling UI and distribution remain. |

## Acceptance status
All 16 rows remain open. The initial audit recovered requirements and reuse. The runnable headless batch module now demonstrates real Cytoflow + native scalar composition and fresh-process terminal reopen on a synthetic immutable fixture; it does not prove recording/live/renderer integration. Missing tests/features are obligations, not waived scope. The first two-recording milestone and complete F01–F16 acceptance are separately pending.

## Research limits
R01–R07 are static/continuation evidence, including incomplete renderer/native edges. R08–R10 are version-specific historical Windows 10.8.1 evidence: 229,585 imported events, 190,705 in one rectangle, two 4,096-bin exports. No boundary events occurred, so that fixture does not prove inclusive boundary behavior. Saved workspace parsing is not an independent reopen test. Exact raw medians differ from histogram-derived medians. No FlowJo 11 numerical parity claim.

The historical Penpot inventory has 20 boards, 214 interactions and three flows. Live pages projection pins revision 23/version 0 and 5,011 shapes only; it omits prototypes and cannot revalidate those 214 interactions. No Penpot edits or vendor source/assets were copied.

## Native/worker slice evidence (2026-10-22)

`analysis.population` and `analysis.graph`: normal and ASan/UBSan pass. Python real-engine adapter: 8 tests pass. Rust host/journal: 6 tests pass including the normally ignored real-engine test. Fresh-process module query reproduces `true, unknown, unknown, false` by exact source/set/row keys. Commands, limits and fixture are in [module README](../../../tools/flowjo_acceptance/README.md). Historical bare-engine qualification remains distinct and intentionally red for its two adapter-required cases. Remote [analysis CI](https://github.com/gavinlouuu-kpt/mib-studio-qt/actions/runs/39762481872) passes at `1278cfe5`, including native sanitizers, host/journal checks, clean pinned engine build and real-engine adapter tests. This is not a claim all repository CI is green.

## Production integration delta (2026-10-23)

The [production API](production-api.md) is implemented in the real Studio/Review
bridges, not a replacement UI. Acceptance remains per-row, not inferred from a
successful checkpoint:

| Rows | Implemented and verified delta | Still open |
|---|---|---|
| F01/F12 | Full-file SHA256 (64 MiB budget), source mismatch refusal, SQLite immutable CAS revisions, receipt replay/restart/rollback, append-only restore, client pending-command journal | Scalable fingerprints, portable manifest/import/relink workflow, groups/metadata/preferences |
| F04 | Native persisted interval/Boolean DAG, exact revision evaluation, raw nullable reductions | Full interactive gate editing, per-source overrides/resync and QC strategy graph |
| F05/F06 | Fingerprint-fenced selected gallery with exact event keys, real stored thumbnails, 12/page | Saved PlotSpec; E retained images require exact provider asset locators (currently absent) |
| F13 | Actual E producer integration, immutable bounded lease snapshots, batch watermark/coverage preservation, live-run/archive parity | Retained live application snapshots across advancing source; hardware EGrabber qualification |
| F14 | Exact pinned plan lookup/evaluation and method triple lookup; absent compatibility stays unknown; no implicit apply | Full findings/endpoint/next-run evidence linkage and comparison design |
| F16 | Production app scheduler, native source freeze, real Cytoflow polygon AND graph eligibility, cancel/reap/join, durable query/reconnect/reopen | Windows process supervision and distribution qualification; other engines/operations |

F02/F03/F07/F08/F09/F10/F11/F15 retain their original obligations above. No scientific
QC, compensation, embedding, report or interchange capability is accepted merely
because source persistence/jobs now exist.

Observed native tests: producer/archive parity and source fencing; strategy host
CAS/fault/restart; shared store/host tests; Review source concurrency. Eight Review
bridge tests pass including saved strategy reopen and gallery key parity. Ten UI
tests pass including journal failure and lost-reply recovery. Real Cytoflow worker
six tests and application scheduler two tests pass. Tailnet production WebSocket
round trip: 10 selected, mean 104.6, 10 matching thumbnail keys and real-engine 10
selected on the synthetic file. Browser automation is currently unavailable (no
browsers registered), so the new controls' click-through is not claimed. TSan
remains host-startup blocked; no sanitizer pass is inferred from normal tests.

### Continued independent work

F08 now has explicit native type-7 quantiles, sample SD, absolute-mean CV and
positive-only geometric mean, plus exact-revision population tables/native CSV.
F12 now has checksummed portable typed manifest export/import through the same
source-verified CAS/receipt path. F13 can retain an immutable normalized source
projection in the metadata DB (900,000-byte cap) and evaluate it after live source
advancement/restart. F10 has deterministic evidence JSON/table export, **not** a
full report-layout/batch designer. These additions narrow the remaining rows;
they do not waive original advanced workflow, comparison, transform, embedding,
interchange or platform qualification requirements.

F05/F11 now include source/revision-bound saved histogram PlotSpec with up to eight
graph-population layers, native exact bins, explicit count/fraction/density
normalization, separately reported outside-range/missing/unknown values and
presentation-only offsets. F02 has source-bound, attributed manual good/bad/unknown
assertions and a native good-only three-valued filter, propagated into job QC
fingerprints and portable manifests. Automated QC qualification, arbitrary plot
families, multi-source overlays and full layout composition remain open.

F04 now includes pinned template applications, interval overrides, explicit resync
by new revision and removed-node migration refusal. F09 includes equal-unit native
descriptive independent/paired comparisons over immutable retained sources, with
source-reuse rejection and explicit missing semantics. F14 includes durable immutable
findings with exact plan/source references and content hash. Inferential fits,
scientific compatibility qualification and I next-run linkage remain open. The
two-recording production backend milestone passes; full F01–F16 acceptance does not.

F10 now has revisioned ordered-block layouts and atomic immutable report runs over
retained table/plot/finding references, exact replay/collision handling, checksum
reopen and standalone HTML/JSON downloads. Arbitrary layout editing and background
multi-report batch scheduling remain open; bounded report composition is not full
F10 acceptance. Tailnet real-app build/reopen and native fault/identity tests pass.

F03 now has explicit ordered derived linear/asinh/log10 parameters, unit validation,
null-domain propagation and parameter fingerprints shared by gates/stats/plots/jobs.
Compensation controls/estimation and Logicle remain unimplemented; no channel or
calibration authority is fabricated. Native and real-engine transformed-input parity
passes through the production tailnet backend.

F07 now has a production supervised seeded t-SNE artifact over two explicit raw or
derived parameters, bounded hash-ranked subsampling, exact event-key mapping,
config/eligibility validation and durable reopen. Same-runtime repeatability passes.
High-dimensional selection, UMAP and cross-platform qualification remain open; this
is not all F07 acceptance. Coordinates are not interpreted as QC or causal evidence.

### Current remaining scope after continued production integration

The initial audit table above is historical; these remaining obligations supersede
its absent-feature wording. No aggregate F01–F16 acceptance is claimed.

| Rows | Current implemented evidence | Remaining implementation/qualification |
|---|---|---|
| F01 | Verified SHA256 relink (10 gates/images unchanged), retained-source labels/tags/groups library, duplicate detection, unit provenance | Fingerprinting beyond 64MiB; broad library discovery/metadata workflow |
| F02 | Durable attributed manual tri-state QC, native eligibility/job propagation | Qualified automated scientific QC and biological validation |
| F03 | Ordered linear/asinh/log10 derived columns, exact units/domain/null semantics, shared worker/native fingerprint | Compensation controls/estimation, Logicle, applicable fluorescence channels |
| F04 | Persisted interval/Boolean/parent DAG, immutable revisions, source-specific overrides/resync, explicit migration refusal | Full graphical gate editing and remaining gate families |
| F05/F11 | Saved native histogram/scatter, sampled display, multi-source overlays, immutable report plotting | Other plot families and graphical layout controls |
| F06 | Fingerprint-fenced stored gallery, exact keys, real thumbnails and relink parity | E retained-image locators absent; live image retention cannot be fabricated |
| F07 | Supervised 2–16-feature seeded t-SNE, explicit sampling, exact map, same-runtime reorder/reopen tests | Other embedding engines, cross-platform qualification |
| F08 | Native raw/derived quantiles/SD/CV/geometric mean and population tables/CSV | Broader reusable table designer and advanced statistics |
| F09 | Explicit experimental units, paired/independent descriptive comparisons, duplicate-source rejection | Scientific compatibility qualification, inferential fits/models |
| F10 | Saved ordered-block layouts and immutable report runs, JSON/standalone HTML | Arbitrary layout designer and cancellable multi-report batch orchestration |
| F12 | Source-bound CAS/restore, checksummed portable strategies, pending receipts, retained sources, library/sample display preference precedence | Full portable workspace lifecycle/global preferences and scalable relink |
| F13 | Pinned E producer live/archive parity, source fencing, immutable retention, coverage semantics | Actual hardware acquisition-budget qualification, TSan host startup failure |
| F14 | Exact plan/method APIs, immutable findings and pinned report evidence; API sent to I | Final I-owned next-run linkage/composition and scientific applicability qualification |
| F15 | Existing native detection FCS export preserved | FCS import, GatingML and qualified per-cell interchange |
| F16 | Durable real app Cytoflow+t-SNE jobs, cancellation/reap/restart, exact parameter/QC pins | Windows supervision and distribution/license decision |

Library and display defaults are non-scientific metadata only. Browser connector
currently has no registered browser, so latest controls lack rendered click-through
acceptance despite builds/backend tests. This is an evidence gap, not a waived task.

F05 continuation: saved native raw/derived scatter now shares exact source/parameter
revision identity and explicit hash-ranked display sampling. Counts/statistics use
all selected observations, with missing coordinates separately reported. Empty and
extreme finite presentation tests pass; multi-source overlays remain open.

F11 continuation: native multi-source saved histogram overlays now require exact
retained references and matching bins/domain/units/normalization, preserve per-layer
denominators and keep offsets presentation-only. Exact overlays embed in immutable
report runs. Tailnet14/22-observation proof, mismatch refusal and sanitizer tests pass.
Scientific compatibility, arbitrary plot/layout families and full graphical editing
remain separate obligations.

F07 further continuation: explicit2–16 raw/derived feature selection is now supported
and pinned in supervised t-SNE jobs. Missing selected-feature rows are separately
counted, never imputed or silently removed from the original population. Production
three-feature proof and legacy artifact reopen pass. Other embedding engines and
cross-platform qualification remain separate, not a high-dimensional selection gap.

F04 continuation: production form-based interval/Boolean editing, reference-safe
rename/delete, draft undo/redo and exact saved-graph loading now preserve CAS and
method/application provenance. Native graph content fingerprints prevent unchanged
draft labels from aliasing edited bounds. Real revision/replay/conflict proof passes.
Full graphical gesture editing and remaining gate families are still open.

F04 rectangle creation now reuses explicit native interval/AND populations, with
real7-selected/7-image proof. Selected/restored revisions require exact loading
before draft save to protect previously unseen method/application provenance.

F08 continuation: reusable source-bound table definitions now persist exact ordered
population/column selections through strategy revisions, applications/manifests,
native CSV and immutable report rendering. Native missing-population/domain-column
refusals and real tailnet two-row projection pass. Advanced estimators and a graphical
table designer remain separate; no formula language is inferred.

F01/F12 continuation: bounded native saved-record discovery and exact library reopen
are now exposed in the production module. Catalog pages list IDs/revisions only,
never fabricate source compatibility or silently follow a moving head. Full workspace
portability and large-source/asynchronous fingerprinting remain open.
