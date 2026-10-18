# One review implementation: MIB Studio's review features on ReviewSession (#512)

Status: active

Date: 2026-10-18. Track B (Review app, #717), step B0. Issue #512 ("One
review implementation"). Branch `refactor/512-one-review` from `develop`
`2a4e0a4f`. Parent plan:
[`2026-10-01-standalone-review-app.md`](2026-10-01-standalone-review-app.md)
(decision A of 2026-10-05: develop's MIB Studio review stack kept until #512).
ADRs: [0004](../../decisions/0004-bridge-contract-and-operation-state.md)
(additive contract; the ABI bumps when the command/event surface changes),
[0011](../../decisions/0011-yofo-studio-pz7035-instrument.md) (ABI numbers
are reserved by the coordinator; Qt is fixes-only),
[0014](../../decisions/0014-yofo-review-on-react-tauri.md) and its
2026-10-06 amendment. Vault: `knowledge_map/services/ReviewSession.md`,
`knowledge_map/frontend/YofoReview.md`, `knowledge_map/frontend/HdfReviewTab.md`,
`knowledge_map/architecture/Rust-Bridge.md`.

## Goal

One review implementation, `ReviewSession` + `ReviewJobs` and the engines
beside them in `mib_review_core`, sits behind every review shell: MIB Studio
(React + Tauri, and YOFO Studio's remote React over the WebSocket server),
YOFO Review, and the Qt `HdfReviewTab` (through the shared kernels; Qt is
fixes-only). `BackendFacade` delegates its review surface to it and the
duplicated `Hdf5Service`-based review state in the facade is gone. MIB
Studio's Review stage mounts the shared module `desktop/src/review/` through
one entry point (step B5). Done when #512's criteria hold: one implementation
behind the three shells, `tools/review_parity/` passes against MIB Studio's
Review tab as well, and the TD-18 row of the tech-debt tracker no longer links
to #512.

## Survey (2026-10-18, develop `2a4e0a4f`)

### The two stacks side by side

| Concern | MIB Studio today (develop #450, `BackendFacade`) | YOFO Review today (`ReviewSession` / `ReviewJobs`, review bridge) |
|---|---|---|
| Open / close | `load_recording` (`handleRecordingLoadCommand`, `BackendFacade.cpp:1009-1106`) loads into `AppBackend::hdf5()`, the experiment writer's handle; `close_review` (`:3896`) closes it | `review_open` / `review_close`: the session's own read-only `Hdf5Service` |
| Source guards | load and close refused unless the experiment is idle (`withIdleConfiguration`) and no raw recording runs ("Stop raw recording before loading review data") | none (no shared handle) |
| Review state | `loadedRecordingPath_`, `reviewMetricsLoaded_`, `reviewValidMeta_`, `reviewInvalidMeta_` under `reviewMutex_` (`BackendFacade.h:1303-1309`) | the session (metadata cached once per open) |
| Metadata | `fetch_review_metadata`: counts, ROI, core identity, background, five dataset infos | `fetch_review_info`: the same plus series, multi-image window, accounting + summary text, recorded factor, KDE JSON, recorded ring range |
| Metrics pages | `fetch_review_metrics_page` → `MonitoringObjectRow` (7 columns shown) | `fetch_review_rows` → every FilterResult column + µm² |
| Images | `fetch_review_image` / `fetch_review_frame_packet`: Mono8, channel 0 | `fetch_review_frame`: Mono8 or RGB8 with the overlay composed; series; thumbnail strips |
| Overlay render | `render_review_overlay` (`ReviewCharts.cpp`): independent reader on the request's `source_path`, `recording::renderProcessingOverlay`, PNG | `review::composeOverlay` (a copy of the same algorithm, Mono8 kept Mono8) |
| ROI drawing | yellow (255, 210, 0), 1 px, unscaled | red 3 px, scaled (the Qt tab's) |
| Charts | `fetch_review_charts_json`: whole-file 128 × 128 density, exact histogram, bundled curves; **live** factor and **live** ring-ratio range | `fetch_review_scatter` (columnar) + density job (KDE levels) + core contour; **recorded** factor (TD-17) and **recorded** ring range |
| Export | `review_export_json`: one generic `HdfExportRequest` (metrics_csv / images / all / charts; frames valid / invalid / both; series; keep partial; explicit destination; 1-256 `source_paths` sharing one request, per-file results); backend-rendered 1200 px chart TIFFs `scatter_plot.tiff` / `histogram.tiff` with an isoelastic toggle; factor = request or live | four fixed jobs: Export Metrics, Export All (shell chart snapshots `scatter_plot.tiff` / `ring_width_histogram.tiff`), Batch (per-source recorded factor, `<base>_metrics_N.csv`, recording files → images), Export Charts |
| Reanalysis | `review_reanalysis_json` (`ReviewReanalysis.cpp`, ~200 lines in the facade): hdf / folder / avi; dataset all (timestamp order) / valid / invalid / recorded; budgets; ROI / config / background overrides; quiet-tile synthetic background; absolute timestamps; `.partial-<job>` + no-clobber publish; refuses an existing output | `review_regenerate_masks`: current valid / invalid / whole file / AVI / folder; recorded config, ROI, background; median-of-32 synthetic background; timestamps rebased to the first frame; writes the output path directly |
| Reanalysis preview | `fetch_review_reanalysis_preview` (independent reader, Mono8) | none |
| Accounting | `fetch_run_accounting("review")` reads the shared handle | in `fetch_review_info` |
| Jobs | one export **and** one reanalysis at a time; facade operation ids (`Export`, `Reanalysis`); status JSON snapshots polled | one job of any kind at a time; own ids; events drained by `poll_review_events` |
| Transport | Tauri (`mib-app-commands`) and the WebSocket server (`dispatch.rs` → `mib-bridge-server`, YOFO Studio on the PZ7035) | Tauri only, YOFO Review binary (`desktop/src-tauri/src/review.rs`) |
| Contract | `bridge-contract.json` (ABI 38): `review_image_datasets`, `review_export_json`, `review_reanalysis_json`, `review_charts_json`, `render_review_overlay`, `review_reanalysis_preview` | `review-contract.json` (`review_abi_version` 1): overlay modes, pixel formats, job kinds, regenerate sources, density, packets |
| UI | inline in `desktop/src/App.tsx` (load/scrub `:916-998`, toolbar and tabs `:2335-2519`) + `exportControls.tsx`, `reanalysisControls.tsx`, `components/ReviewCharts.tsx`, `components/SavedReviewImage.tsx`, `reviewExport.ts` | `desktop/src/review/` (`ReviewApp` → `ReviewPanel`), not mounted by MIB Studio |

### Findings

- **F1 Two overlay kernels.** `recording::renderProcessingOverlay`
  (`mib_processing`; behind the Qt `OverlayRenderer` and the facade's PNG) and
  `review::composeOverlay` (`mib_review_core`) were the same algorithm copied;
  they differed only in returning an undrawn Mono8 original as Mono8.
  Resolved by S1.
- **F2 Three ROI styles became two:** Qt and ReviewSession draw red 3 px
  scaled; the facade draws yellow 1 px unscaled. Decision D2.
- **F3 Factor policy.** The facade uses the live processing factor (charts;
  export default; `App.tsx` passes the live factor for single-file exports and
  nothing for batches). Qt and YOFO use each file's recorded factor (TD-17,
  closed 2026-10-04). Decision D3.
- **F4 Histogram range.** Facade and Qt follow the live config; YOFO the
  recorded one (an accepted Qt/YOFO difference since 2026-10-04). Decision D4.
- **F5 Export shapes differ** (table above): MIB Studio has Export Images,
  frame-class selection, keep-partial and per-file batch results; YOFO has
  per-source factors and shell-rendered charts. Decision D5.
- **F6 Three regenerate semantics.** Facade: absolute timestamps, quiet-tile
  background, no-clobber publish. YOFO: rebased timestamps, median-of-32
  background, direct write. Qt `BatchMaskDialog`: rebased timestamps,
  quiet-tile background, direct write. Decision D6.
- **F7 The shared handle leaks into experiments.** While MIB Studio has a
  review file loaded, `AppBackend::hdf5()` is open, so experiment readiness
  fails `storage.hdf5` ("an HDF5 file is already open") and Start is refused
  (`ExperimentCoordinator.cpp:917`, `:1029`) until Close File. Decision D1.
- **F8 Concurrency differs:** facade one export + one reanalysis at once;
  ReviewJobs one job of any kind. Decision D7.
- **F9 Transport.** MIB Studio's review commands also serve YOFO Studio's
  remote React over the WebSocket server; the review bridge commands do not.
  `desktop/src/review/` imports `@tauri-apps/api/core`,
  `@tauri-apps/plugin-dialog` and `@tauri-apps/plugin-opener` directly instead
  of `desktop/src/transport/`, and chart staging (`review_stage_chart`) uses a
  raw Tauri IPC body.
- **F10 The Qt tab** (frozen, ADR 0011) already runs on the shared kernels:
  `Hdf5Service`, `HdfExportService`, `makeReviewChartData`,
  `OverlayRenderer` → `renderProcessingOverlay`,
  `ReviewSession::recordedPixelToMicron`, and `BatchMaskDialog` →
  `processBatch` / `saveMasksToHdf5`.
- **F11 Parity measurement** covers Qt ↔ YOFO only (`tools/review_parity/`);
  `frontend.review_parity` (`tests/frontend/review_parity_test.cpp`, Qt
  build) compares the Qt overlay with the facade PNG and the facade charts with
  `makeReviewChartData`. Nothing measures MIB Studio's Tauri tab against Qt
  or YOFO.
- **F12 Stale comments.** `review/ReviewPanel.tsx`, `review/ReviewApp.tsx` and
  `review/reviewBridge.ts` said MIB Studio mounts the panel / calls the review
  bridge. It does not (`App.tsx` builds its own tab). Fixed in this PR
  (comments only).
- **F13 Out of scope:** FCS export (#574) exists only in the Qt tab; neither
  React shell has it, and #512 does not add it.
- **F14 A dead second CSV writer.** `review::writeMetricsCsv`
  (`include/backend/recording/ReviewExport.h`, compiled into
  `mib_review_core`) has no callers since the facade's export moved to
  `HdfExportService`; S3 deletes it.

## Target design

- **Engines in `mib_review_core`:** one overlay kernel (S1), the reanalysis
  engine (S2), the export engine (S3), the charts-JSON and saved-overlay
  builders (S4). Each parses and formats the bridge contract's JSON where a
  JSON command exists, so the facade and the review bridge can expose the same
  shapes without re-implementing them.
- **The facade owns one `std::shared_ptr<review::ReviewSession>`** (S5) and
  its review jobs run on `ReviewJobs` lanes (S7). The guards are facade
  policy; the session is mechanism. Every existing MIB bridge review command
  stays (ADR 0004: additive only) as a thin delegate, so YOFO Studio's remote
  client and old desktop builds keep working.
- **The review bridge attaches to the facade's session** in MIB Studio and
  YOFO Studio (S10) and owns its own session in YOFO Review, so the review
  module talks to one contract (`review-contract.json`) in every product.
- **MIB Studio's Review stage is the review module** (S13, step B5) through
  one entry point, `desktop/src/review/index.ts` → `<ReviewModule host={…}/>`.

## Decisions needed

Owner (Gavin) or the merge coordinator decides; each slice says which it
waits for. Recommendations are this plan's.

- **D1 Experiments while a review file is open** (F7, gates S5). (a) Keep
  today's behaviour: the facade tells the readiness gate a review source is
  open and the `storage.hdf5` message stays. (b) Allow experiments while
  reviewing, since the review reader no longer touches the writer's handle.
  Recommended: (b), landed as its own visible change inside S5 with the
  readiness test updated; (a) is the byte-identical fallback.
- **D2 One ROI style** (F2, gates S4's facade output and S13). Recommended:
  Qt's red 3 px scaled (already Qt and YOFO, parity-signed 2026-10-04).
- **D3 Factor policy for MIB Studio** (F3, gates S13). Recommended: each
  file's recorded factor with the live factor as the fallback, as Qt and
  YOFO. The facade's JSON commands keep their live default until the contract
  text of `review_charts_json.calibration` ("current backend pixel_to_micron,
  same as Qt") is revised by the coordinator.
- **D4 Histogram range** (F4, gates S13). Recommended: the recorded range
  (YOFO), with the live range only for files without a recorded config.
- **D5 Exported chart files** (F5, gates S12/S13). Recommended: the module's
  shell snapshots (Qt file names) for MIB Studio after B5; the facade's
  `review_export_json` keeps backend-rendered charts for its clients.
- **D6 One regenerate semantics** (F6, gates S8). Recommended: one engine
  (S2) with two options, `timestamps: preserve | rebase` and
  `syntheticBackground: quietTile | medianOf32`; facade = preserve +
  quietTile, YOFO = rebase + medianOf32 (its owner-accepted difference of
  2026-10-04); both get the partial file + no-clobber publish (a change for
  YOFO: an existing output is refused instead of replaced).
- **D7 Job lanes** (F8, gates S7). Recommended: two lanes per session, exports
  and processing (reanalysis / regenerate / density / core), single flight per
  lane: MIB Studio keeps export-while-reanalysing; YOFO gains it.
- **D8 Transport for the module in MIB Studio and YOFO Studio** (F9, gates
  S10-S12). Recommended: attach the review bridge to the facade (S10) and
  register the review commands in `mib-app-commands` for the Tauri studio
  build and the WebSocket server (S11). Alternative: a second, unattached
  review bridge in the studio build with the guards checked in Rust (racy
  against an experiment start, two open files, no shared accounting).
- **D9 What "one implementation behind the Qt tab" means** (F10, gates S14).
  Recommended: the Qt tab calls the same Qt-free kernels and is otherwise
  untouched (ADR 0011 fixes-only); recorded here and in ADR 0014's
  amendment at close-out.

## Slices

One PR each, in order. "Contract" names the bridge contract
(`bridge-contract.json`, ABI 38 on develop) and the review contract
(`review-contract.json`, `review_abi_version` 1). Every slice keeps
`python3 scripts/check_docs.py` and `python3 scripts/gen_bridge_contract.py --check`
green and lands its vault updates. ABI numbers in the proposals are left to
the coordinator (ADR 0004/0011).

| # | Slice | Bridge contract | Review contract | Status |
|---|---|---|---|---|
| S1 | One overlay kernel | none | none | done on this branch |
| S2 | Reanalysis engine in `mib_review_core` | none | none | done on this branch |
| S3 | Export engine in `mib_review_core` | none | none | planned |
| S4 | Charts JSON and saved-overlay builders | none | none | planned |
| S5 | The facade owns the ReviewSession | none (behaviour D1) | none | planned |
| S6 | Parity harness: a MIB Studio half | none | none | planned |
| S7 | Facade jobs on ReviewJobs lanes | none | none | planned |
| S8 | YOFO regenerate on the reanalysis engine | none | none (behaviour D6) | planned |
| S9 | Transport-neutral review module | none | none | planned |
| S10 | Attachable review bridge | **additive, ABI bump** | none | proposed, not implemented |
| S11 | Review commands in the studio command layer | **additive command surface, ABI bump** | none | proposed, not implemented |
| S12 | MIB Studio's options in the review contract | none | **additive, ABI bump** | proposed, not implemented |
| S13 | B5: MIB Studio's Review stage mounts the module | none | none | planned (B5) |
| S14 | Close-out | none | none | planned |

### S1 — One overlay kernel (done on this branch)

- **Moves:** `review::composeOverlay` stops carrying its own copy of the
  overlay algorithm and calls `recording::renderProcessingOverlay` with a new
  option `OverlayPassthrough::KeepMono` (an undrawn Mono8 original stays
  Mono8, which ReviewSession's packets and thumbnails rely on). The default
  `OverlayPassthrough::Rgb` keeps the Qt and facade output unchanged.
- **Files:** `include/backend/recording/ProcessingOverlay.h`,
  `src/backend/recording/ProcessingOverlay.cpp`,
  `include/backend/review/OverlayCompose.h`,
  `src/backend/review/OverlayCompose.cpp` (static_asserts pin the two
  `OverlayMode` enums), new `tests/review/review_overlay_kernel_test.cpp`
  (`review.overlay_kernel`), `tests/CMakeLists.txt`, vault
  `services/ReviewSession.md`.
- **Contract:** none.
- **Proof:** `review.overlay_kernel` runs 864 cases (3 originals × 8 masks × 6
  classes × 6 modes) and requires `composeOverlay` to be byte-identical to the
  pre-#512 implementation (kept in the test as the oracle), every drawn pixel
  to equal the Qt/facade kernel's, and a Mono8 result to be the undrawn
  original; a mutation (KeepMono → Rgb) fails 139 checks. `review.session`,
  `review.jobs`, `backend.export_bridge_facade`, `backend.facade_boundary`
  pass unchanged. A before/after characterization run (temporary, not
  committed) of both kernels over the matrix plus the facade's reanalysis
  cases was identical.
- **Risks:** low; ROI drawing untouched (D2).

### S2 — Reanalysis engine in `mib_review_core` (done on this branch)

- **Moves:** the facade's reanalysis request parsing, the worker body (load,
  budgets, overrides, synthetic background, `processBatch`, index / timestamp /
  track remap, partial file, no-clobber publish, cleanup), the preview read and
  the status JSON move verbatim into `backend/review/Reanalysis.h`
  (`parseReanalysisRequest`, `runReanalysis`, `loadReanalysisPreview`,
  `reanalysis*StatusJson`). The facade keeps the policy: the live config as
  the default, single flight, the `Reanalysis` operation, the status
  snapshot, the thread.
- **Files:** new `include/backend/review/Reanalysis.h`,
  `src/backend/review/Reanalysis.cpp`; `src/backend/app/ReviewReanalysis.cpp`
  (now delegates); `src/backend/CMakeLists.txt` (`mib_review_core` sources);
  new `tests/review/review_reanalysis_test.cpp` (`review.reanalysis`);
  `tests/CMakeLists.txt`; vault `services/ReviewSession.md`,
  `frontend/HdfReviewTab.md`.
- **Contract:** none (the JSON shapes and messages are unchanged).
- **Proof:** `backend.export_bridge_facade` (all its reanalysis cases:
  fractional / negative ranges, existing output, cancel cycles, missing
  source, write fault, folder / AVI ranges, selected background, late
  collision) passes unchanged; `review.reanalysis` drives the engine with no
  `AppBackend`: parse defaults / every field / 13 exact refusal messages,
  round trips (all, range, folder), cancel before and during, budget, missing
  source, late collision, the status strings byte for byte, the preview. A
  mutation (timestamps not preserved) fails both tests. The characterization
  run (19 rejections, 14 reanalysis cases, h5dump of 7 outputs, normalized
  for wall-clock and temp paths) was identical before and after.
- **Risks:** platform publish code (`renameat2`, `MoveFileExW`, hard-link
  fallback) now compiles in the review core on macOS and Windows; the review
  CI lanes (`review-ci.yml`) must stay green there.

### S3 — Export engine in `mib_review_core`

- **Moves:** the facade's `submitReviewExportJson` worker (one
  `HdfExportService` run per source, per-file results, the batch rule
  "continue on failure, stop on cancel", the final state) and its JSON parse /
  status formatting into `backend/review/ExportEngine.h`
  (`parseExportRequest(json, defaults)` where defaults carry the host's factor
  and chart ring range, `runExportBatch(request, sources, cancel, progress)` →
  per-file outcomes, `exportStatusJson(…)`). `ReviewJobs::startExportMetrics /
  startExportAll / startBatchExport` build their requests as today (recorded
  factor per source, shell chart snapshots, `_metrics_N.csv` names) and run
  through the same engine. The unused `review::writeMetricsCsv` (F14) goes.
- **Files:** new `include/backend/review/ExportEngine.h`,
  `src/backend/review/ExportEngine.cpp`; `src/backend/app/BackendFacade.cpp`
  (`submitReviewExportJson`, `handleReviewCommand`); `src/backend/review/ReviewJobs.cpp`;
  `src/backend/CMakeLists.txt`; deleted `include/backend/recording/ReviewExport.h`,
  `src/backend/recording/ReviewExport.cpp`; new `tests/review/review_export_engine_test.cpp`;
  vault `services/ReviewSession.md`, `services/HdfExportService.md`.
- **Contract:** none.
- **Proof:** `backend.export_bridge_facade` and `review.jobs` (its byte
  equality with a direct `HdfExportService` run) unchanged; the new test pins
  the status JSON of single, batch, cancelled and failed runs; a temporary
  characterization of the facade's status JSON and output trees before/after;
  `tools/review_parity/run.sh` on the review fixtures (Qt ↔ YOFO) unchanged.
- **Risks:** progress phase strings and the `results` array are consumed by
  `exportControls.tsx`; the `keep_partial_on_failure` manifest path.

### S4 — Charts JSON and saved-overlay builders

- **Moves:** `fetch_review_charts_json`'s body (`makeReviewChartData`, the
  128 × 128 density, bins, curves, the JSON) and `render_review_overlay`'s
  body (independent reader, 64 MiB limit, classification, overlay, ROI, PNG)
  into `backend/review/ReviewCharts.h` as
  `reviewChartsJson(rows, source, factor, ringMin, ringMax)` and
  `renderSavedOverlayPng(requestJson, RoiStyle)`. The facade keeps its
  policy (loaded file, live factor and range, `RoiStyle::Facade` until D2).
- **Files:** new `include/backend/review/ReviewCharts.h`,
  `src/backend/review/ReviewCharts.cpp`; `src/backend/app/ReviewCharts.cpp`;
  `src/backend/CMakeLists.txt`; new test `review.charts_json`; vault.
- **Contract:** none.
- **Proof:** `frontend.review_parity` (Qt build: Qt QImage = facade PNG for the
  five modes; chart axes and bins = `makeReviewChartData`),
  `backend.export_bridge_facade` (chart snapshot, overlay PNG); the new test
  compares the JSON string with the pre-slice output on a fixture.
- **Risks:** low.

### S5 — The facade owns the ReviewSession (source ownership)

- **Moves:** `BackendFacade` gains `std::shared_ptr<review::ReviewSession> review_`.
  `RecordingLoad`, `closeReview`, `fetchReviewMetadata`,
  `fetchReviewMetricsPage`, `fetchReviewImage`, `fetchRunAccountingJson("review")`,
  `fetchReviewChartsJson` and the export default source read the session. The
  guards ("Experiment must be idle before loading review data", "Stop raw
  recording before loading review data", the close equivalents) stay in the
  facade around `review_->open()` / `close()`. Deleted: `loadedRecordingPath_`,
  `reviewMetricsLoaded_`, `reviewValidMeta_`, `reviewInvalidMeta_`,
  `reviewMutex_` and every review use of `backend_.hdf5()` (load, close,
  metadata, pages, images, accounting, shutdown). `ReviewSession` gains
  `framesPage(valid, offset, count, frames, total)` (metadata-only
  `ProcessedFrame`s) for the `MonitoringObjectRow` mapping. D1 decides whether
  `storage.hdf5` keeps tripping on a review file.
- **Files:** `include/backend/app/BackendFacade.h`,
  `src/backend/app/BackendFacade.cpp`, `src/backend/app/ReviewCharts.cpp`,
  `include/backend/review/ReviewSession.h`, `src/backend/review/ReviewSession.cpp`,
  `src/backend/app/ExperimentCoordinator.cpp` only under D1 (a), tests below,
  vault `services/ReviewSession.md`, `architecture/Rust-Bridge.md`,
  `architecture/Desktop-Shell.md`.
- **Contract:** none for the bridge (the metadata, page, image and
  accounting shapes are filled from the session field for field).
- **Proof:** `backend.facade_boundary`, `backend.export_bridge_facade`,
  `frontend.review_parity`; cargo `tests/contract.rs` (`record_then_load_and_review`
  and the load cases around lines 447-472, 1001, 1170, 1343, run with
  `MIB_BRIDGE_NO_CMAKE=1`); `review.session` (+ `framesPage`); a new facade
  test: load while the writer handle is open is refused only by the guards,
  the writer handle is untouched by load / close, the metadata and pages equal
  the pre-slice output on the fixtures; the experiment readiness test
  (`backend.experiment_readiness`) for D1.
- **Risks:** anything that relied on the review file sitting in the shared
  handle (F7; the Qt shell does not use the facade); shutdown order (jobs
  joined before the session closes); YOFO Studio's remote client sees the same
  JSON.

### S6 — Parity harness: a MIB Studio half

- **Moves:** `tools/review_parity/` gains a third half: cargo example
  `crates/mib-bridge/examples/review_parity_studio.rs` over `BackendBridge`
  (`load_recording`, `review_export_json` metrics_csv / all, `render_review_overlay`,
  `fetch_review_charts_json`) writing `studio/summary.json`, `studio/metrics.csv`,
  `studio/all/`; `compare.py` reports studio ↔ qt and studio ↔ yofo with D2-D5
  listed as ACCEPTED until S13.
- **Files:** `tools/review_parity/{run.sh,compare.py,README.md}`, the new
  example, vault `frontend/YofoReview.md`.
- **Contract:** none.
- **Proof:** the harness itself on the sign-off inputs (the `z-adjustment-50v`
  corpus, the 512x96 run of `integration.review_scatter_e2e`, the review
  fixtures); results in the PR.
- **Risks:** needs the Qt build for the Qt half (`linux-system-release`).

### S7 — Facade jobs on ReviewJobs lanes

- **Moves:** `ReviewJobs` gains lanes (D7) and host hooks: the host supplies
  the operation id and cancel flag (the facade's `beginOperation`) and
  receives Started / Progress / terminal events, so `cancel_operation`, the
  `Export` / `Reanalysis` operation kinds and shutdown cancellation work as
  today. The facade's `exportThread_`, `reanalysisThread_`,
  `reviewJobThreads_`, `exportActive_`, `reanalysisActive_` go; the status
  snapshots become views of the lane state built with the S2 / S3 formatters.
- **Files:** `include/backend/review/ReviewJobs.h`,
  `src/backend/review/ReviewJobs.cpp`, `BackendFacade.{h,cpp}`,
  `src/backend/app/ReviewReanalysis.cpp`, `crates/mib-bridge/src/review_shim.cpp`
  (lane-aware single flight), tests, vault.
- **Contract:** none.
- **Proof:** `backend.export_bridge_facade` (cancel cycles, duplicate refusal,
  status), `review.jobs`, the TSan lane (`sanitizers.yml`) and a new
  concurrency stress test (coverage matrix: threads / shared state), cargo
  `tests/review_bridge.rs`.
- **Risks:** event ordering (Started before Progress; one terminal state per
  id); the late-event policy of ADR 0004.

### S8 — YOFO regenerate on the reanalysis engine

- **Moves:** `ReviewJobs::startRegenerateMasks` builds a `ReanalysisRequest`
  (recorded config / ROI / background; the D6 options) and runs
  `runReanalysis`; its own load / background / write code goes.
- **Files:** `include/backend/review/Reanalysis.h` (options),
  `src/backend/review/Reanalysis.cpp`, `src/backend/review/ReviewJobs.cpp`,
  `tests/review/review_jobs_test.cpp`, vault `frontend/YofoReview.md`
  ("Differences from the Qt tab").
- **Contract:** none (`review_regenerate_sources` unchanged).
- **Proof:** `review.jobs` regenerate cases, `review.reanalysis` with the new
  options, cargo `tests/review_bridge.rs`.
- **Risks:** the no-clobber change for YOFO (D6) needs the YOFO save dialog to
  stop offering "replace".

### S9 — Transport-neutral review module

- **Moves:** `desktop/src/review/` calls `invoke` and the dialogs through
  `desktop/src/transport/` (and the opener only when not remote), and chart
  staging gets a transport method for binary uploads, so the module can run
  over the WebSocket transport.
- **Files:** `desktop/src/review/{reviewBridge.ts,ReviewPanel.tsx,RegenerateMasks.tsx,charts/ChartsView.tsx,charts/chartExport.ts,exports/ExportDialogs.tsx}`,
  `desktop/src/transport/{Transport.ts,tauriTransport.ts,wsTransport.ts}` for
  the upload method (coordinate with the transport owner), vitest.
- **Contract:** none (same Tauri commands).
- **Proof:** `npx vitest run`, `npx tsc --noEmit`, the review build under Xvfb.
- **Risks:** `transport/` is shared with the operation app (track A).

### S10 — Attachable review bridge (proposed; not implemented)

Needs the coordinator's ABI number. Exact proposed change:

- `crates/mib-bridge/src/lib.rs`, module `ffi`, inside `unsafe extern "C++"`:

  ```rust
  /// The review bridge over this backend's ReviewSession: the facade's review
  /// commands and the review module share one open file. `review_open` through
  /// it is refused unless the experiment is idle and no raw recording runs
  /// (load_recording's guards and messages); its jobs run on the facade's
  /// review lanes and are cancelled and joined by the facade's shutdown.
  #[namespace = "mib_review_bridge"]
  type ReviewBridge = crate::review_ffi::ReviewBridge;
  fn attach_review_bridge(self: Pin<&mut BackendBridge>) -> UniquePtr<ReviewBridge>;
  ```

- `crates/mib-bridge/contract/bridge-contract.json`: `"abi_version": <reserved>`
  and a top-level entry `"attach_review_bridge": "-> review bridge
  (review-contract.json) over the facade's ReviewSession; one open file for
  load_recording / fetch_review_* and review_open / fetch_review_info; open
  refused unless the experiment is idle and no raw recording runs (same
  messages as load_recording); jobs on the facade's review lanes"`.
- `crates/mib-bridge/src/shim.{h,cpp}`: `attach_review_bridge()`;
  `bridge_abi_version()` returns the reserved number; the ABI history comment
  gains "v<N> added attach_review_bridge (#512)".
- `crates/mib-bridge/src/review_shim.{h,cpp}`: a `ReviewBridge` constructor
  over a shared session + lanes + an open-policy callback (C++ only, no
  review contract change).
- Tests: `tests/contract.rs` (attach; `review_open` via the attached bridge is
  seen by `fetch_review_metadata`; refused during raw recording);
  `tests/review_bridge.rs` unchanged.

### S11 — Review commands in the studio command layer (proposed; not implemented)

Same ABI bump as S10 if they land together. Exact proposed change:

- Move the command bodies of `desktop/src-tauri/src/review.rs` into a module
  usable by both products without `mib_backend`
  (`crates/mib-app-commands/src/review.rs` behind a feature, or a small
  `crates/mib-review-commands` crate) and register, in the Tauri studio build
  and in `crates/mib-app-commands/src/dispatch.rs` `COMMANDS` (WebSocket
  server): `review_abi_version`, `review_open`, `review_close`,
  `review_set_pixel_to_micron`, `fetch_review_info`, `fetch_review_rows`,
  `fetch_review_frame`, `fetch_review_series_count`, `fetch_review_series_packet`,
  `fetch_review_thumbnails_packet`, `fetch_review_scatter`,
  `review_save_core_record`, `review_export_metrics`, `review_export_all`,
  `review_export_charts`, `review_stage_chart`, `review_clear_charts`,
  `review_list_dir`, `review_batch_export`, `review_regenerate_masks`,
  `review_compute_core`, `fetch_review_computed_core_json`,
  `review_request_density`, `fetch_review_density`, `review_jobs_busy`,
  `poll_review_events`, `cancel_review_operation`, `fetch_isoelastic_curves`.
  None collides with an existing name.
- `bridge-contract.json`: a top-level entry `"review_commands": "the
  review-contract.json command set, also served by MIB Studio and the
  WebSocket server over attach_review_bridge"`; the ABI number as S10.
- Risks: `review_list_dir` and the dialogs on a remote instrument browse the
  board's file system; `review_stage_chart` needs the S9 upload over WS.

### S12 — MIB Studio's options in the review contract (proposed; not implemented)

Needs a reserved `review_abi_version`. Exact proposed change (JSON
passthrough, reusing the S2 / S3 parsers so the shapes stay those of the
bridge contract):

- `crates/mib-bridge/src/review_bridge.rs`, `review_ffi`:

  ```rust
  /// bridge-contract.json `review_export_json` (request and status, same
  /// validation) over this session; a job of kind `Export` (7).
  fn review_export_json(self: Pin<&mut ReviewBridge>, json: &str) -> ReviewResult;
  fn review_export_status_json(self: Pin<&mut ReviewBridge>) -> String;
  /// bridge-contract.json `review_reanalysis_json`; a job of kind `Reanalysis` (8).
  fn review_reanalysis_json(self: Pin<&mut ReviewBridge>, json: &str) -> ReviewResult;
  fn review_reanalysis_status_json(self: Pin<&mut ReviewBridge>) -> String;
  /// bridge-contract.json `review_reanalysis_preview`: Mono8.
  fn fetch_review_reanalysis_preview(self: Pin<&mut ReviewBridge>, json: &str) -> ReviewFrame;
  ```

- `crates/mib-bridge/contract/review-contract.json`:
  `"review_abi_version": <reserved>`; `"review_operation_kinds"` appends
  `"Export": 7, "Reanalysis": 8`; a new group `"json_requests":
  {"review_export_json": "bridge-contract.json review_export_json, request and
  status verbatim", "review_reanalysis_json": "bridge-contract.json
  review_reanalysis_json, request and status verbatim",
  "fetch_review_reanalysis_preview": "bridge-contract.json
  review_reanalysis_preview; pull kind review (3), Mono8"}`.
- Regenerated `desktop/src/review/reviewContract.ts` and
  `desktop/src-tauri/src/review_packet_contract.rs`
  (`scripts/gen_bridge_contract.py`); `review_shim.cpp` static_asserts for
  kinds 7 and 8; `tests/review_bridge.rs`; Tauri commands in `review.rs`
  (and S11's list).

### S13 — B5: MIB Studio's Review stage mounts the module

- **Moves:** `desktop/src/review/index.ts` exports the single entry point
  `ReviewModule` (props: `ready`, `scheduler`, `fitWindow`, `log`,
  `beforeLoad`, `onFileChange`, `onInfo`, `fallbackPixelToMicron`,
  `capabilities` {reanalysis, coreUpdates…}, and an imperative handle for
  menus). YOFO Review's `ReviewApp` mounts it; `App.tsx`'s Review stage
  renders `<ReviewModule …/>` instead of the inline tab and stops mounting
  its own "review" scheduler slot. The module gains MIB Studio's options over
  S12 (Export Images, frame classes, keep partial, reanalysis budgets /
  preview / ROI drawing / background selection / config draft), each behind
  a capability flag.
- **Files:** `desktop/src/review/**` (track B), `desktop/src/App.tsx` (track
  A's file: the PR is reviewed with its owner), vitest, the review build
  under Xvfb, `docs/manual/review-and-postprocess.md`.
- **Contract:** none (uses S10-S12).
- **Proof:** vitest; `tsc --noEmit`; S6's harness run on the module path
  (the studio half switches from the facade JSON commands to the attached
  review bridge) with D2-D5 resolved; Xvfb click-through of MIB Studio and
  YOFO Studio's remote client.
- **Risks:** feature regressions for MIB Studio operators; App.tsx
  ownership; the remote file dialogs (S11).

### S14 — Close-out

- **Moves:** delete `exportControls.tsx`, `reanalysisControls.tsx`,
  `components/ReviewCharts.tsx`, `components/SavedReviewImage.tsx`,
  `reviewExport.ts` and their tests (track A review); the facade's JSON review
  commands stay as delegates (ADR 0004) and are marked superseded in
  `bridge-contract.json` text at the coordinator's next renumber; TD-18's row
  drops the #512 link (MIB Studio's charts are the module's canvas); ADR 0014
  amendment marked resolved; D9 recorded; this plan → `completed/`.
- **Proof:** `tools/review_parity/run.sh` passes with the studio half;
  `check_docs.py`; `check_screenshots.py`.

## Acceptance criteria

- [x] S1: one overlay kernel behind Qt, the facade and ReviewSession.
- [x] S2: the reanalysis engine lives in `mib_review_core`; the facade
      delegates.
- [ ] S3-S9 land with no bridge or review contract change.
- [ ] S10-S12 land with coordinator-reserved ABI numbers.
- [ ] `BackendFacade` holds no review state of its own and never touches
      `AppBackend::hdf5()` for review.
- [ ] MIB Studio's Review stage is `desktop/src/review/` through one entry
      point; the inline App.tsx tab and its components are gone.
- [ ] `tools/review_parity/` passes Qt ↔ YOFO ↔ MIB Studio on the sign-off
      inputs.
- [ ] TD-18 no longer links to #512.

## Progress

- [x] 2026-10-18 — plan; stale ReviewPanel / ReviewApp / reviewBridge header
      comments fixed (F12).
- [x] 2026-10-18 — S1 one overlay kernel (`review.overlay_kernel`).
- [x] 2026-10-18 — S2 reanalysis engine (`review.reanalysis`).
- [ ] S3 export engine
- [ ] S4 charts JSON and saved-overlay builders
- [ ] S5 facade owns the ReviewSession (D1)
- [ ] S6 parity harness MIB Studio half
- [ ] S7 facade jobs on ReviewJobs lanes (D7)
- [ ] S8 YOFO regenerate on the engine (D6)
- [ ] S9 transport-neutral review module
- [ ] S10 attachable review bridge (ABI)
- [ ] S11 review commands in the studio command layer (ABI)
- [ ] S12 MIB Studio's options in the review contract (review ABI)
- [ ] S13 B5 mount
- [ ] S14 close-out
