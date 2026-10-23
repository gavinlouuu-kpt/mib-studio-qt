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
