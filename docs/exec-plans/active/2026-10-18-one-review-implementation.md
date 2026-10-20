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

**Status 2026-10-19.** All nine decisions are ruled (merge coordinator, on #717). D2, D5,
D7, D8 and D9 were accepted as recommended on 2026-10-18; D7 is amended below. On
2026-10-19 Gavin delegated D1, D3, D4 and D6: D3, D4 and D6 are accepted as recommended,
and D1 is ruled (a), against the recommendation, because HDF5 is built without thread
safety (#722). No slice waits on a decision; S13 still needs S10–S12.
On 2026-10-20 the coordinator added D10, an accepted difference that S6 found
and that lasts until S13.

- **D1 Experiments while a review file is open** (F7, gates S5). (a) Keep
  today's behaviour: the facade tells the readiness gate a review source is
  open and the `storage.hdf5` message stays. (b) Allow experiments while
  reviewing, since the review reader no longer touches the writer's handle.
  **Ruled (a), 2026-10-19.** HDF5 1.14.6 is built with `threadsafe=False` (the
  Conan default) and `Hdf5Service` has no process-wide lock, so a review reader
  beside the 5 kHz recorder would race inside the library, besides competing
  for disk and CPU. S5 keeps the refusal byte-identical. Revisit after #722.
- **D2 One ROI style** (F2, gates S4's facade output and S13). Recommended:
  Qt's red 3 px scaled (already Qt and YOFO, parity-signed 2026-10-04).
- **D3 Factor policy for MIB Studio** (F3, gates S13). **Accepted.** Recommended: each
  file's recorded factor with the live factor as the fallback, as Qt and
  YOFO. The facade's JSON commands keep their live default until the contract
  text of `review_charts_json.calibration` ("current backend pixel_to_micron,
  same as Qt") is revised by the coordinator. Ruling: the facade commands keep
  the live default; the stale "same as Qt" is dropped (description only, no ABI
  bump) by the slice that next touches `review_charts_json`.
- **D4 Histogram range** (F4, gates S13). **Accepted.** Recommended: the recorded range
  (YOFO), with the live range only for files without a recorded config.
- **D5 Exported chart files** (F5, gates S12/S13). Recommended: the module's
  shell snapshots (Qt file names) for MIB Studio after B5; the facade's
  `review_export_json` keeps backend-rendered charts for its clients.
- **D6 One regenerate semantics** (F6, gates S8). **Accepted.** Recommended: one engine
  (S2) with two options, `timestamps: preserve | rebase` and
  `syntheticBackground: quietTile | medianOf32`; facade = preserve +
  quietTile, YOFO = rebase + medianOf32 (its owner-accepted difference of
  2026-10-04); both get the partial file + no-clobber publish (a change for
  YOFO: an existing output is refused instead of replaced).
- **D7 Job lanes** (F8, gates S7). Recommended: two lanes per session, exports
  and processing (reanalysis / regenerate / density / core), single flight per
  lane: MIB Studio keeps export-while-reanalysing; YOFO gains it.
  **Amended 2026-10-19:** the lanes stand, but YOFO does not gain the second
  lane before #722 makes concurrent HDF5 use safe; MIB Studio's existing
  export-while-reanalysing is listed in #722 as at risk.
  **Per-host lane policy (S7, as built):** `ReviewJobsOptions` with one named
  preset per host. `forReviewBridge()` (YOFO Review, `review_shim.cpp`, and
  the default): `concurrentLanes = false` (one job of any kind; a comment
  points at #722), ReviewJobs' own ids, the lane free before the terminal
  event. `forFacade(host)` (MIB Studio, `BackendFacade`): `concurrentLanes =
  true` (one export and one reanalysis at once, as before), the facade's
  operations through `ReviewJobHost::beginOperation`, the lane free after the
  terminal event. Tested per preset by `review.jobs_lanes`; the facade's by
  `backend.facade_review_jobs` too. Revisit `forReviewBridge` after #722.
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
- **D10 Histogram y-axis headroom** (found by S6, #755). **Accepted until S13,
  coordinator ruling 2026-10-20.** MIB Studio's inline Review tab ends the
  histogram y axis at the largest bin (`desktop/src/components/ReviewCharts.tsx`
  `maxBin`). Qt applies `applyNiceNumbers()` over `ceil(1.1 × max)`, and YOFO
  uses `ceil(1.1 × max)`. This is display headroom only; the bins and counts
  are identical. `tools/review_parity/compare.py` accepts it only when the
  bins are identical and each top is its shell's rule. S13 removes it when
  MIB Studio mounts the shared module. (The ruling called it D6, but D6 is
  "One regenerate semantics" above, so it is D10 here.)

## Slices

One PR each, in order. "Contract" names the bridge contract
(`bridge-contract.json`, ABI 38 on develop) and the review contract
(`review-contract.json`, `review_abi_version` 1; 2 since B1a, below). Every slice keeps
`python3 scripts/check_docs.py` and `python3 scripts/gen_bridge_contract.py --check`
green and lands its vault updates. ABI numbers in the proposals are left to
the coordinator (ADR 0004/0011).

| # | Slice | Bridge contract | Review contract | Status |
|---|---|---|---|---|
| S1 | One overlay kernel | none | none | merged (#721) |
| S2 | Reanalysis engine in `mib_review_core` | none | none | merged (#721) |
| S3 | Export engine in `mib_review_core` | none | none | merged (#724) |
| S4 | Charts JSON and saved-overlay builders | none (text only, D3) | none | merged (#726) |
| S5 | The facade owns the ReviewSession | none (D1 (a): unchanged) | none | merged (#733) |
| S6 | Parity harness: a MIB Studio half | none | none | in review (#755) |
| S7 | Facade jobs on ReviewJobs lanes | none | none | in review (#742) |
| S8 | YOFO regenerate on the reanalysis engine | none | none (behaviour D6) | merged (#735) |
| S9 | Transport-neutral review module | none | none | merged (#731) |
| S10 | Attachable review bridge | **additive, ABI bump** | none | proposed, not implemented |
| S11 | Review commands in the studio command layer | **additive command surface, ABI bump** | none | proposed, not implemented |
| S12 | MIB Studio's options in the review contract | none | **additive, ABI bump** | proposed, not implemented |
| S13 | B5: MIB Studio's Review stage mounts the module | none | none | planned (B5) |
| S14 | Close-out | none | none | planned |

### B1a — Run identity in the review contract (track B step B1; not a #512 slice)

R1 Open's backend (#717 B1, #269), on `develop` because it bumps the review
ABI; review ABI 2 was reserved by the coordinator on #717 (2026-10-19), so
S12 takes 3 or higher. Additive; the bridge ABI is untouched.

- **Identity:** `ReviewMetadata::runIdentity` / `fetch_review_info.identity`:
  snapshot version, run id, experiment id, run index, sample ref,
  calibration record id, operator and the annotation, read from run snapshot
  v3 (#712) and `/experiment_annotation`. Absent members are absent (`has_*`
  false over the bridge, `null` in JSON), never 0 or ""; an attribute that is
  not a JSON object is "unreadable", not legacy.
- **Probe:** `review_probe_runs(paths)`: header attributes only (identity,
  times, counts, accounting, core version), at most 256 paths
  (`review_probe.max_paths`), per-file errors. Each count and time is read on
  its own (review round 1): a missing count is absent, a recorded 0 stays 0.
  Serialized with the session's
  HDF5 access (#722): each file under `ReviewSession`'s mutex, and the whole
  call under the job lanes' mutex (#512 S7) with a refusal while either lane is claimed
  (`ReviewJobs::probeRuns`), because jobs read outside the session's mutex.
- **Proof:** `review.run_identity` (v3 / legacy / v1 / unreadable / corrupt
  files, the cap, a probe while a file is open, probe and reads on two
  threads, the lane gate); `tests/review_bridge.rs` in both feature
  configurations; the Tauri JSON test; the `reviewIpc` golden gains the probe
  call. No UI change; B1b uses it on `feat/egrabber-workspace`.

### S1 — One overlay kernel (merged, #721)

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

### S2 — Reanalysis engine in `mib_review_core` (merged, #721)

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

### S3 — Export engine in `mib_review_core` (in review, #724)

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
- **As built (2026-10-19):** `exportStatusJson(…)` is four formatters,
  `export{Validating,Progress,Terminal,StartFailed}StatusJson`, as S2's.
  `runExportBatch` takes `ExportRunOptions`: `prepare` (per-source request,
  or skip) carries ReviewJobs' batch rules, which must run just before each
  source (the `_metrics_N.csv` name depends on the files already written);
  `runExportFile` is the single run ReviewJobs' Export Metrics / All use.
  Not anticipated: the two hosts cancel differently. The facade hands the
  operation's flag to the service as its token (polled at every check);
  ReviewJobs copies its flag into a private token at each progress report, so
  a cancel lands one report later and its event sequence differs
  (`ExportCancelPolling::Direct` / `AtProgress`, both kept; S7 can unify
  them). A single-source export cancelled before it started reports
  `"error":""` (kept).
- **Proof (#724):** `review.export_engine` (new): parse defaults / every
  field / 12 exact refusal messages; single All and metrics-batch round trips;
  status snapshots byte for byte (validating, progress, terminal of single,
  batch, cancelled, failed, cancel-before-start, start failure); continue on
  failure, stop on cancel, `prepare` skips; image write fault with and without
  `keep_partial_on_failure` (manifest), a kept cancel; both polling modes and
  ReviewJobs keeping `AtProgress`. Mutations (`file_index` off by one, the
  before-source cancel check removed; ReviewJobs switched to `Direct`) fail
  it. `backend.export_bridge_facade`, `review.jobs`, `review.session`,
  `review.reanalysis`, `review.overlay_kernel`, `backend.facade_boundary`
  pass unchanged. A temporary characterization (not committed) of the facade
  (20 refusals, 19 export runs incl. batch, cancel, keep-partial, faults, the
  legacy `ExportMetricsCsv` command) and of ReviewJobs (12 export jobs incl.
  batch skips and cancels): every status snapshot, operation / job event and
  the output tree (paths, sizes, content hashes) was identical before and
  after, normalized only for temp paths and job ids.

### S4 — Charts JSON and saved-overlay builders (in review, #726)

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
- **As built (2026-10-19):** `reviewChartsJson` and `renderSavedOverlayPng` in
  `backend/review/ReviewCharts.h`, plus `reviewChartsErrorJson` (the
  `{"valid":false,"error":…}` shape). The rows are still read by the facade
  (loaded file, own reader; S5 moves that to the session). `RoiStyle` has
  `Facade` (used) and `Review` (`drawRoiOverlay`, the Qt / ReviewSession
  pixels), so D2's switch later is one argument. Per the D3 ruling this slice
  drops the stale "same as Qt" from `review_charts_json.calibration` in
  `bridge-contract.json` (description only, no ABI bump; the generated
  contracts are unchanged). Not anticipated: `makeReviewChartData` throws for an
  empty or reversed ring range, which the facade has always reported as
  `{"valid":false,"error":"Invalid chart calibration or histogram range"}`
  (kept).
- **Proof (#726):** `review.charts_json` (new) keeps the pre-slice facade
  code as the oracle: 20 chart cases (5 row sets incl. NaN / Inf / extremes
  and bin edges × 4 factor / range settings, 5 of them refused) and 852
  overlay requests (5 files × valid / invalid × 6 indices × modes −1..5 × ROI,
  plus 12 malformed or default requests) are byte-identical or throw the same
  type and message; a small fixture's JSON is pinned literally; the Review
  ROI style equals `ReviewSession::fetchImage` for modes 0..4. Mutations (the
  facade ROI colour, the `rows` counter, the Review ROI scale) fail it.
  `backend.export_bridge_facade`, `backend.facade_boundary` and the review
  tests pass unchanged. A temporary characterization (not committed) of the
  facade (`fetchReviewChartsJson` on 6 files × 2 live settings and with no
  file; `renderReviewOverlayJson` on 852 requests) was identical before and
  after.

### S5 — The facade owns the ReviewSession (source ownership; in review, #733)

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
- **As built (2026-10-19):** as planned; the facade's `review_` is a
  `const std::shared_ptr` made in the constructor. The load event's counts,
  the metadata, the "review" accounting and the charts' rows come from what
  the session read at open (the same `Hdf5Service` calls the shared handle
  answered per request). D1 (a) is `AppBackend::setReviewSourceOpen(bool)` /
  `reviewSourceOpen()`: the facade sets it from `review_->isOpen()` inside the
  idle transaction (load, failed load, close; cleared at shutdown and in the
  destructor) through a scope guard that also runs on an exception, and the
  coordinator's `storage.hdf5` gate and Start check read
  `hdf5().isFileOpen() || reviewSourceOpen()` under the coordinator mutex, so
  the gate id, status, reason and remediation and the Start message are
  unchanged. Shutdown closes the session after the export and reanalysis jobs
  are joined and no longer closes the shared handle (its writers close it).
  Not anticipated: (1) raw recording also refused on the shared handle
  (`AppBackend::startFrameRecordingLocked`, "Close the existing HDF5 file
  before starting raw recording; its writer will not be replaced"); without
  the hook it would have been admitted beside a review reader, so it reads the
  flag too, with the same message. (2) The session read the valid metadata at
  open without remembering whether it was absent or unreadable, which the
  charts tell apart ("Cannot read valid-frame metrics"):
  `ReviewSession::validSet()`, which returns the path, file type, readability
  and rows in one snapshot. (3) Before S5,
  `fetch_review_metrics_page` and `fetch_review_image` checked only that the
  shared handle was open, so with no review file during a run they read the
  writer's file on the caller's thread; they now fail without a review file.
  (4) `framesPage` is overflow-safe for a huge `count`; the facade's old page
  (and `ReviewSession::metricsPage`, unchanged) compute `offset + count`.
  Review of #733 (2026-10-19) found two defects, both fixed: (5) a metadata
  read that threw inside `open()` after the reader opened (allocation, or
  OpenCV on a malformed background) skipped the flag and released the
  coordinator mutex with the reader open, a D1 window; `open()` is now all or
  nothing (the reader is closed again, false, "Failed to read <path>: …") and
  the facade's flag is set by a scope guard on every exit. (6) The charts read
  path, file type, readability and rows with separate locks, so a concurrent
  load could chart one file's rows under another's identity, or let a
  recording past the experiment-only check; they now take one `validSet()`
  snapshot. The re-review found three more, fixed: (7) `Hdf5Service::loadFile`
  could leak a handle when the path copy after `H5Fopen` threw (the open state
  still false, so the guard would clear the refusal); it now releases the
  handle and rethrows, and `ReviewSession::isOpen()` fails closed on any handle
  the reader holds (`Hdf5Service::holdsFileHandle()`). (8) `closeFile()` built
  diagnostics after `H5Fclose` but before clearing its state; it now clears
  the handle and state right after `H5Fclose`, wraps every diagnostic in
  `try`, and closes any handle held, also one whose open did not complete.
  (9) The charts stress test could run zero fetches; a handshake now makes
  the loader wait for the first fetch.
- **Proof (#733):** `backend.facade_review_session` (new) keeps the pre-slice
  facade code as the oracle: load result, RecordingStatusEvent, operation and
  error events, metadata, 14 pages per file, every dataset × 5 indices,
  "review" accounting and charts JSON are equal on experiment, legacy,
  empty-valid, unreadable-metadata and two recording fixtures, a
  `readMetadata = false` load and three failed loads (missing, not HDF5, empty
  path) that close the open file; the export's default source is the session's
  file; a writer handle open beside it is not closed or replaced by load,
  replace or close and keeps writing; the `storage.hdf5` gate of every loaded
  file equals the shared handle's field for field, Start is refused with the
  pre-slice message, and after Close File (or a failed load, or shutdown) the
  gate passes. `review.session` adds `framesPage` (equal to the reader's own
  metadata read and to `metricsPage`, bounds, zero and huge counts) and absent
  vs unreadable valid sets, `validSet` and the all-or-nothing open (a
  malformed background whose read throws inside `Hdf5Service`: closed, no
  throw, the error names the file, the next open works). The facade test also
  loads that file from closed and after a good load: the flag equals the
  session's state and the load fails closed with its failed-load events (with
  the pre-fix load path the flag reads closed while the reader is open and
  Start passes the writer check; with only the rollback removed, the guard
  still keeps the flag equal and Start refused); and it charts on one thread
  while another loads full / legacy / a recording 300 times: every answer is
  one file's whole answer (probabilistic: the separate-lock accessors were caught
  in 2 of 3 runs at 120 loads; the deterministic proof is the `validSet` unit test).
  The final round adds `recording.hdf5_resilience`
  (`testHandleNeverLeaksOnThrow`: a throw injected right after `H5Fopen`, and
  one in `closeFile`'s diagnostics, leave no handle, the library's open-file
  count back at baseline, `closeFile` not throwing) and `review.session` (the
  same open-time throw through the session: closed, no handle, the file named;
  a reader handle held behind the session's back makes `isOpen()` true while
  nothing is served, and `close()` / the next `open()` release it), with the
  test hooks `Hdf5ServiceTestAccess` / `ReviewSessionTestAccess`. Mutations
  (no release in `loadFile`, the close hook before the state is cleared,
  `isOpen()` without the handle) fail them.
  `backend.experiment_readiness` section 5b: with the
  flag set the gate equals the shared handle's, every other gate is unchanged,
  Start and raw recording are refused with the unchanged messages, and once
  cleared the instrument is ready and raw recording starts. `backend.facade_boundary`:
  the raw-recording and idle guards' exact messages with the writer handle
  open and untouched, and at the facade with a live camera the D1 gate, Start
  and raw-recording refusals beside a loaded file, then Close File and a real
  experiment run. Mutations (the gate without the flag; the load not setting
  it; the readability rule reduced to the read result) fail the new tests.
  A temporary characterization (not committed) of the facade on 15 files (the
  six fixtures and nine real runs, among them the 512x96 review-scatter run
  and the review-parity inputs): load events, metadata, pages, image hashes,
  accounting, charts, the export's default source and CSV hashes, the
  `storage.hdf5` gate, the Start and raw-recording refusals, close and the
  guards during a raw recording, 1844 lines, identical before and after.
  cargo `tests/contract.rs` 27 passed (incl. `record_then_load_and_review`,
  `recording_load_emits_operation_lifecycle`,
  `review_metadata_pages_images_and_export_job`) and `tests/review_bridge.rs`
  with `MIB_BRIDGE_NO_CMAKE=1`. `frontend.review_parity` needs the Qt build:
  left to CI's Linux Qt lane.

### S6 — Parity harness: a MIB Studio half (in review, #755)

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
  `makeReviewChartData` pads the chart ranges in `long double`: 80-bit on
  Linux GCC, 64-bit on MSVC and arm64 macOS, so ranges can differ in the last
  bits across platforms. Compare chart ranges with a tolerance, or pin them
  per platform (found in S4, #726).
- **As built (2026-10-20):** `review_parity_studio.rs` runs each file twice
  through the facade. *As opened* (`studio/charts_app.json`, summary `app`):
  the live factor and ring range of a fresh backend (0.4886, 15–25), MIB
  Studio's policy today, so D3 / D4 are measured. *Compared outputs*
  (`charts.json`, `metrics.csv`, `all/`, `overlays/`): the live factor and range
  are first set to the file's recorded ones, through `apply_processing_config_json`,
  as the Qt dump sets its live config; they are read through the review bridge
  (`ReviewSession`, TD-17's reader), and a file without them keeps the live
  values (D3 / D4's fallback). The exports are the requests `exportControls.tsx`
  builds with its defaults (frames both, all series, isoelastic overlays, keep
  partial, the live factor), polled through `review_export_status_json`. The
  overlays are `SavedReviewImage.tsx`'s request for valid / invalid frames 0,
  middle and last × modes 0..4 × ROI off / on. Under `--features review-only`
  the example is a stub that exits 2, so the review-only `cargo test` lanes
  still build every example; Bridge CI's `cargo test` builds the real one. The
  YOFO half also dumps `fetch_review_frame` overlays (`yofo/overlays/`, PGM /
  PPM) for the same frames, and its summary carries the recorded ROI. A third
  studio pass, the *D4 probe* (`charts_d4.json`), sets the live ring range to
  the recorded one ± 5, so every input exercises D4. `compare.py` keeps the Qt
  ↔ YOFO table as signed off and adds MIB Studio ↔ Qt and MIB Studio ↔ YOFO
  tables. Each acceptance is checked against its effect, not only the declared
  input (two Codex reviews of #755). D2–D5 and D10 are ACCEPTED only under
  these conditions:
  - **D2:** both halves report the same recorded ROI, and each paints exactly
    the band `cv::rectangle` paints for its own style: every band pixel in
    its colour, no other pixel changed. MIB Studio: yellow (255, 210, 0),
    thickness 1 on `Rect & image`, the 1 px path. YOFO: red, thickness 3 on
    `drawRoiOverlay`'s clamped rectangle, which OpenCV (LINE_8) paints as a
    5 px band with rounded outer corners. `compare.roi_band` models that band;
    it matched `cv2.rectangle` on 6237 thickness-3 rectangles (clamped, small
    and degenerate ones included), and the unit test checks it against cv2
    goldens (`tests/tools/data/review_parity_roi_bands.json`). A half that
    draws nothing, draws elsewhere, or draws an incomplete band is a DIFF. The
    halves may differ only on those pixels, and no difference is a PASS. The
    ROI-off renders must be pixel-identical.
  - **D3:** the as-opened charts declare the live factor, and their area
    extents equal the compared pass's × (live / compared)², with the
    deformability extents and the count unchanged.
  - **D4:** the chart declares the live range, and its bins equal YOFO's ring
    ratios binned over that range. With equal ranges the as-opened row reads
    "D4 not exercised" and still checks the bins.
  - **D5:** Export All holds exactly `scatter_plot.tiff` and `histogram.tiff`
    beyond the other half's byte-identical files.
  - **D10:** the bins are identical, and each y top is its shell's rule: MIB
    Studio max(1, max bin), YOFO max(1, ceil(1.1 × max)), Qt
    `applyNiceNumbers()` over that (Qt's `looseNiceNumbers`, 5 ticks). The Qt
    rule reproduces the observed 80 / 300 / 10.

  The scatter density is compared cell by cell with YOFO's points binned on
  MIB Studio's grid. Every point off a cell edge must be in its own cell; a
  point within 1e-9 of an interior cell edge may be in either cell beside that
  edge, and nowhere else (an assignment of the edge points to the cells'
  surplus). Against Qt only the total is compared.

  Each studio table starts with *expected artefacts*, which lists every dump
  file and summary key its checks read and is a DIFF naming each absent one,
  and *studio run*, a DIFF when the example recorded a failure or an export
  did not complete. A missing D4 probe is a DIFF ("D4 probe missing"), not a
  skipped row.

  The chart-range tolerance is `CHART_RANGE_REL` = 1e-12 of the range's
  largest absolute end (about 4500 ulp; the padding differs by a few ulp at
  most). Qt's ring range is read back from its bar labels at 0.05. Not
  anticipated:
  - (1) the facade returns RGB for an undrawn Mono8 frame, while YOFO keeps
    Mono8 (S1's `KeepMono`). The renders are compared as displayed RGB.
  - (2) on `population_3000`, YOFO's double `paddedExtent` and the shared long
    double kernel differ by 1 ulp in the x and y minima. This is the S6 risk on
    Linux itself, and the tolerance absorbs it.
  - (3) a difference outside D2–D5, the histogram y axis. The coordinator
    ruled it D10 (above).
  - (4) the Qt ↔ YOFO accounting row passes now: Qt's status line carries the
    accounting suffix, so the 2026-10-04 acceptance no longer applies.
  - (5) D4 is not exercised as opened: every input records 15–25, the live
    default. Hence the D4 probe.
  - (6) the Windows lane's default cp1252 encoding: `compare.py` reads and
    writes UTF-8.
- **Proof (#755):** the harness on the four sign-off inputs: the
  `z-adjustment-50v` recording, `review_e2e_512x96_run.h5`,
  `synthetic_fixture.h5` and `population_3000.h5`. `run.sh` exits 0. Outputs
  and `run.log` are in
  `/mnt/hdd/developer-data/mib-studio-qt/review-parity/out-2026-10-20-s6-r3/`
  (classifier after the second Codex review). The run after the first review
  is `out-2026-10-20-s6-r2/`; the first run, with the y-axis DIFF and the
  classifier before review, is `out-2026-10-20-s6/`; the 2026-10-04 `out/` is
  untouched.
  - **Identical to Qt and YOFO:** metrics.csv; Export All apart from the chart
    TIFFs (30878 / 587 / 45 / 3301 files); scatter point count and density
    total; scatter axis extents; histogram bins; the compared histogram range
    and px→µm.
  - **Identical to YOFO:** the density cells (314 / 8 / 1735) and the 30
    ROI-off overlays per experiment file. No point of any input lies within
    1e-9 of a cell edge, so every point is in its own cell.
  - **Expected artefacts and studio run:** PASS on every file and pair (159
    checks in all: 40 Qt ↔ YOFO, 55 MIB Studio ↔ Qt, 64 MIB Studio ↔ YOFO; no
    DIFF).
  - **ACCEPTED:**
    - D2: 30 of 30 ROI-on renders per experiment file, each style's full band
      at the recorded ROI: [0, 0, 512, 96], [4, 2, 20, 16] and [0, 0, 64, 48].
    - D3: as-opened px→µm, 0.4886 against 0.25, on the two fixtures, with
      the area extents × 3.81968.
    - D4: the probe binned over [10, 30] on the three experiment files.
    - D5: the two chart TIFFs on the three experiment files.
    - D10: the histogram y axis. MIB Studio / Qt / YOFO tops are 64 / 80 /
      71 on the 512x96 run, 248 / 300 / 273 on population and 8 / 10 / 9 on
      the synthetic fixture, with identical bins.
  - **Not exercised:** D4 as opened (every file records 15–25).
  - **Unit test:** `scripts.review_parity_compare`
    (`tests/tools/test_review_parity_compare.py`, 15 cases) covers:
    - the PNG / PGM / PPM decoders (every PNG filter type);
    - the pinned tolerance (a few ulp pass, 1e-10 relative fails);
    - each acceptance both ways, including the refusals the reviews asked
      for: a missing ROI, an ROI at an offset, an incomplete band (a thin
      1 px red rectangle, a 3 px band, a missing pixel, square corners), a
      declared but unapplied live factor or range (as opened and in the
      probe), every point moved into cell (0, 0), a non-edge point moved
      (Codex's (10, 10) → (90, 90) beside an edge point, refused; an edge
      flip, accepted), and bins that differ (D10);
    - the ROI band model against the seven cv2 goldens, and the ROI geometry;
    - absent dump files (the D4 probe, metrics.csv, charts, scatter dump,
      overlays, `all/`), recorded failures and an export that failed;
    - an unaccepted bin difference failing `main`;
    - a dump without the studio half.
  - **Mutations that fail the unit test:**
    - D2: band colour not required, changes outside the band allowed, square
      corners, or a 3 px band for thickness 3;
    - D3 without the applied check;
    - D4 without the binned check, or the not-exercised row without it;
    - density: any edge point excusing other moves, edge candidates on one
      side only, no assignment, or by its total;
    - D10 without the bins check or without the other shell's rule;
    - a 1e-9 or 1e-6 tolerance;
    - D5 for any non-frame TIFF;
    - the D4 probe row skipped when its evidence is missing, no artefact
      list, overlays not listed, the studio run row always passing.

    The density stray-cell check is an equivalent mutant: the assignment
    implies it, and it is kept for its message.
    Tick count 6 instead of 5 in the Qt rule is an equivalent mutant: the nice
    step of r / 4 equals that of r / 5 for every nice range r.
  - **Decoders:** cross-checked against Pillow on 120 renders.

### S7 — Facade jobs on ReviewJobs lanes (in review, #742)

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
- **As built (2026-10-19):** `ReviewJobs` has two lanes (`ReviewJobLane`
  Export / Processing, `laneOf(kind)`), single flight per lane, and
  `ReviewJobsOptions` {`concurrentLanes`, `laneRelease`, `host`} with the two
  presets recorded under D7. The facade's workers moved in verbatim as the
  JSON jobs `startExportJson` / `startReanalysisJson` (kinds `Export` = 7 and
  `Reanalysis` = 8 in the C++ enum, S12's proposed values; not in
  `review-contract.json`, the review bridge never starts them); each lane
  holds its job's status snapshot (`exportStatusJson()` /
  `reanalysisStatusJson()`, the S2 / S3 formatters written at the same points
  as before). The facade builds its `ReviewJobs` in the constructor with
  `beginOperation` as the host hook (its operation ids, cancel flags,
  `Export` / `Reanalysis` kinds) and a sink that maps Progress / terminal to
  `reportOperationProgress` / `finishOperation`; `shutdown()` calls
  `cancelAllOperations` then `ReviewJobs::shutdown()` (same order as the two
  joins before). Gone from the facade: `exportThread_`, `reanalysisThread_`,
  `exportActive_`, `reanalysisActive_`, `exportMutex_`, `reanalysisMutex_`,
  `exportStatusJson_`, `reanalysisStatusJson_`. `review_shim.cpp` passes
  `forReviewBridge()` explicitly. The two export cancel models (`Direct` for
  the JSON export, `AtProgress` for ReviewJobs' own exports) are kept: unifying
  them would change YOFO's event sequence. Not anticipated: (1)
  `reviewJobThreads_` held no review job; it carried the pump address scans
  and stage-operation waiters, so it stays, renamed `operationWorkers_`. (2)
  `AppBackend::processing()` exists only after `AppBackend::initialize`, so
  `startReanalysisJson` takes the kernel per start (the facade passes the
  live service). (3) The hosts freed their single-flight state on opposite
  sides of the terminal event (the facade after `finishOperation`, so a start
  from inside the terminal event was refused; ReviewJobs before, so `busy()`
  reads false when YOFO receives it); kept per host as
  `ReviewLaneRelease`. (4) Fixed on the way, unreachable or unobservable
  before: ReviewJobs emitted Started under its mutex (a sink could not call
  back in); a start from inside a job's own terminal event self-joined
  (`std::system_error`, swallowed) and now detaches the finished worker; a
  worker that could not be created left the lane busy forever with no
  terminal, and now reports Failed and frees it; a facade destroyed without
  `shutdown()` with a joinable job thread ended the process, and now cancels
  and joins its lanes first. (5) The validating / loading snapshot is visible
  only between the start's return and the worker's first progress (no seam);
  the stress cycles check that the lane's snapshot names the new operation as
  soon as the start returns.
- **Proof (#742):** `backend.facade_review_jobs` (new) keeps the pre-S7 facade
  job code as the oracle (the two submit bodies, flags, threads, snapshots and
  the operation registry, copied from develop 88e9afbe) and drives both through
  11 scenarios with handshakes: complete, cancel at the first progress event
  with a duplicate from inside it, explicit CSV, batch with a missing file,
  charts, reanalysis complete / cancelled / missing source, an export run to
  completion while a reanalysis is held, cancel through
  `dispatch(OperationCommand)` during and after, a start from inside the
  terminal event; all 209 trace lines (events with their status snapshot,
  results, refusals, cancels, snapshots) are equal. Then 12 cycles with both
  lanes held, duplicates refused on both, one lane cancelled while the other
  waits, and shutdown with both held (the reanalysis cancelled once with
  "Backend shutdown", the export's own terminal, nothing after shutdown).
  `review.jobs_lanes` (new): both presets through `ReviewJobsTestAccess`
  bodies, refusals per lane and policy, cancel by id and through the host's
  flag, the release order and a start from inside the terminal event,
  `cancelOverridesOutcome`, shutdown with the lanes held, 25 rounds of 16
  racing starts, 200 mixed cycles per policy (complete / cancel one / cancel
  both / shutdown / a cancel racing the end), one terminal per operation and
  Started first. Mutations fail them: the bridge preset with concurrent lanes,
  the lane policy ignored in `start`, the facade preset releasing before the
  terminal event (also in the oracle test: the start from inside the terminal
  event is accepted, 246 trace lines against 209), the
  self-join guard removed, shutdown not setting the flags, no validating
  snapshot. `backend.export_bridge_facade`, `review.jobs`,
  `review.export_engine`, `review.reanalysis`, `review.session`,
  `backend.facade_review_session`, `backend.facade_boundary`,
  `backend.pump_bridge_facade`, `backend.stage_bridge_facade` pass unchanged.
  A temporary characterization (not committed) of the facade (12 scenarios
  incl. shutdown with an export and a reanalysis held) and of the review
  bridge's ReviewJobs (9 scenarios: every job kind, cross-lane refusals,
  cancels, shutdown with a held export, a start after it), 331 lines, was
  identical before and after in 5 runs each. Cargo `tests/contract.rs` (28)
  and `tests/review_bridge.rs` (2, also with `--features review-only`) pass
  with `MIB_BRIDGE_NO_CMAKE=1`. The TSan lane is CI's (`sanitizers.yml`; the
  new tests carry the `backend` label).
- **Review (#742, 2026-10-19):** (P2) a shutdown from inside a job's own
  event detached that worker, and if the owner then destroyed ReviewJobs while
  the callback still ran, the worker released its lane in freed memory and the
  sink it was executing was destroyed under it. The runner state is now shared
  with the lane workers (`impl_` is a `shared_ptr`, each worker holds a
  reference; job bodies reach the runner only through it, never through the
  ReviewJobs object), so it outlives the object until the worker ends; never a
  self-join. `review.jobs_lanes` calls `shutdown()` inside the terminal event
  and destroys ReviewJobs before the callback returns, for both presets: the
  sink must still be alive when the callback resumes and be released once the
  worker ends (with the worker not holding the runner it fails and crashes).
  (P3) the new tests took "no active operation plus a sleep" as both lanes
  free, but `finishOperation` removes the operation before the terminal event
  and the lane is freed only after it returns. `ReviewJobsTestAccess::waitIdle`
  waits on the lane release itself (`BackendFacadeTestAccess::reviewJobs`
  reaches the facade's lanes; the oracle joins its worker); no sleep stands in
  for synchronisation (job bodies poll their cancel flag, which has no
  condition variable). The characterization was rerun on the fixed code:
  identical in 3 runs.
- **Review round 2 (#742, 2026-10-19):** the coordinator ruled that destroying the owner (ReviewJobs, BackendFacade) from inside its own callback is out of contract; callbacks may call `shutdown()` / `cancel()` (now written on ReviewJobs and the facade). A start keeps its own reference to the runner state. A start that a `shutdown()` overtakes reports one Cancelled terminal without running its body (the JSON jobs: their engine's outcome for a run cancelled on entry). `shutdown()` no longer waits for a start in progress, because that wait deadlocked against a terminal-event `shutdown()` under the bridge's policy.
- **Review round 3 (#742, 2026-10-19):** `shutdown()` from outside the runner (the owner's teardown) waits again for every start claimed before it, until it has returned with its Cancelled reported; from a lane worker or a callback it does not wait (a thread-local marker per runner). The outside wait cannot deadlock: a start in progress runs only ReviewJobs' code and those callbacks and joins its lane's previous worker, and a shutdown from either is the inside case.

### S8 — YOFO regenerate on the reanalysis engine (in review, #735)

- **Moves:** `ReviewJobs::startRegenerateMasks` builds a `ReanalysisRequest`
  (recorded config read by the job; recorded ROI / background read by the
  engine; the D6 options) and runs `runReanalysis`; its own load / median /
  remap / write code goes (the median-of-32 moved into the engine). The
  request gets three options, all left at the facade's values by parsing:
  `timestamps: Preserve | Rebase`, `syntheticBackgroundKind: QuietTile |
  MedianOf32`, and `frameOrder: Timestamp | Datasets` (plus `convertColorToGray`,
  see Review below). D6 named two
  options; the third keeps YOFO's whole file as valid rows then invalid
  rows, the "timestamp order" difference accepted on 2026-10-04 (without it
  rows reorder and the rebase origin moves). YOFO keeps no budget, its range
  clamp and its message format. An existing output is refused at start with
  the facade's message ("Output already exists; choose a new file").
- **Found:** the pre-S8 job paired records with frames **by position**.
  `processBatch` emits one record per track (fewer records than frames when
  tracking merges frames) or per object. So on real cells most records
  carried another frame's index and timestamp, and track first/last frames
  stayed batch positions. The engine pairs by the record's frame (as Qt and
  the facade do), which fixes it. Accepted by the coordinator; the existing
  files are tracked as #737.
- **Review (#735):** `frameOrder` kept (ruled). Colour HDF frames: the
  pre-S8 reader (`loadFromHdf5`) converted 3-channel frames BGR → gray, which
  the engine refused. A fourth option, `convertColorToGray` (YOFO on, facade
  off), keeps that. `review.jobs` compares a colour fixture's output with the
  pre-S8 job, kept as the oracle.
- **Dialog:** `RegenerateMasks.tsx` proposes a free name
  (`defaultRegenerateName`, `_2`, `_3` … via `review_list_dir`; names
  compared ignoring case, the search bounded, with a timestamped fallback,
  #735 review). It stays
  open on a refusal, shows "Not started: <backend reason> (<file>)", clears
  the output and asks again. It closes once the job starts
  (`useReviewJobs.start` hooks `onRefused` / `onStarted`). The OS save panel's
  own overwrite question cannot be turned off through the dialog plugin;
  confirming it replaces nothing now.
- **Files:** `include/backend/review/Reanalysis.h`,
  `src/backend/review/Reanalysis.cpp`, `include/backend/review/ReviewJobs.h`,
  `src/backend/review/ReviewJobs.cpp`, `tests/review/review_jobs_test.cpp`,
  `tests/review/review_reanalysis_test.cpp`,
  `crates/mib-bridge/tests/review_bridge.rs`, `desktop/src/review/`
  (`RegenerateMasks.tsx`, `ReviewPanel.tsx`, `exports/useReviewJobs.ts`,
  `exports/exportHelpers.ts`, tests, IPC golden), vault
  `frontend/YofoReview.md`, `services/ReviewSession.md`, manual
  `yofo-review.md`.
- **Contract:** none (`review_regenerate_sources` unchanged; the refusal is
  the existing not-started result with a message).
- **Proof:** `review.jobs` has a tracked-ring fixture where 10 frames give 6
  records. It checks that every record's image is its frame's, the rebase
  and clamp, valid-then-invalid order, source-index track frames, the range
  clamp, the median-of-32 against the pre-S8 code (kept as the oracle), the
  start refusal (the original untouched), a mid-run collision (job fails,
  file untouched, no partial) and cancel (nothing written). Mutations fail it:
  facade options for YOFO fail 4 checks; positional pairing fails 4.
  `review.reanalysis` covers the options (parse keeps the defaults, rebase
  offsets and clamp, dataset order, median vs quiet tile). Cargo
  `review_bridge.rs` covers the refusal over the bridge and a regenerate into
  a new file. The desktop IPC golden gains `review_list_dir` before the save
  dialog and a refused-then-renamed flow (additions only).
- **Characterization** (temporary harness, not committed; h5diff before /
  after). YOFO regenerate covers 19 cases: synthetic experiment / recording /
  folder / AVI fixtures and the real-cell `review_e2e_512x96_run.h5` and
  `512x96stream-mock-frames`. Hdf outputs with one record per frame are
  identical. Folder and AVI outputs differ only in the wall-clock
  `experiment_info` start / end. Real-cell hdf outputs have identical
  images, masks and background; only metadata differs: the pairing fix
  (1040 of 1049 records misnamed before, 0 after; files regenerated up to
  review-v1.1.3-beta.1 are affected, #737) and `end_time_ns`. Messages
  are unchanged except the empty range ("Selected source range is empty").
  The facade path (12 cases) has identical status JSON and outputs, except the
  wall-clock attributes of folder and AVI outputs.
- **Risks:** low. The YOFO save dialog's flow changed (refusal stays in the
  dialog). A folder or AVI with an unreadable image now fails, as the facade
  does, instead of skipping it.

### S9 — Transport-neutral review module (in review, #731)

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
- **Landed (2026-10-19):** every `invoke`, dialog and opener call under
  `desktop/src/review/` goes through `desktop/src/transport/` (`updates.tsx`
  too); `reviewBridge` gained `clearCharts` / `stageChart`. Transport,
  additive only: `Transport.upload(cmd, bytes, headers)` (Tauri:
  `invoke(cmd, bytes, {headers})`; WebSocket: rejects with
  `TransportUnsupportedError`, since the server reads no binary requests),
  `transport/dialogs` `ask`. Over the WebSocket, Show in folder is disabled
  with its reason and a refused chart upload is shown in plain words. Proof:
  `reviewIpc.test.tsx` compares every call at the Tauri boundary with a golden
  file recorded before the move, unchanged after it;
  `transportBoundary.test.ts` is the import guard; `remoteTransport.test.tsx`
  checks the browser behaviour. The Xvfb boot smoke was not run locally; it
  runs in `review-ci.yml`. No contract change.

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
  board's file system. `review_stage_chart` needs a binary request path in
  `mib-bridge-server`, because client binary frames are only proof of life
  today. Once that exists, `wsTransport.upload` replaces its
  `TransportUnsupportedError` with a real send (the client side is S9's).

### S12 — MIB Studio's options in the review contract (proposed; not implemented)

Needs a reserved `review_abi_version` (3 or higher: B1a took 2). Exact proposed change (JSON
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
- [x] 2026-10-19 — S1 and S2 merged (#721); D1, D3, D4 and D6 ruled, D7 amended (#717, #722).
- [x] 2026-10-19 — S3 export engine (`review.export_engine`, #724); `review::writeMetricsCsv` deleted (F14).
- [x] 2026-10-19 — S4 charts JSON and saved-overlay builders (`review.charts_json`, #726).
- [x] 2026-10-19 — S5 the facade owns the ReviewSession (`backend.facade_review_session`; D1 (a) via `AppBackend::setReviewSourceOpen`, gate text unchanged; #733).
- [x] 2026-10-20 — S6 parity harness MIB Studio half (`review_parity_studio.rs`, three-table `compare.py`, D2–D5 and D10 accepted until S13, each checked by its effect; `run.sh` exits 0; #755).
- [x] 2026-10-19 — S7 facade jobs on ReviewJobs lanes (D7 per-host policy: `forReviewBridge` one job at a time until #722, `forFacade` both lanes; `backend.facade_review_jobs` with the pre-S7 oracle, `review.jobs_lanes`; #742)
- [x] 2026-10-19 — S8 YOFO regenerate on the engine (D6; `review.jobs`, `review.reanalysis`, no clobber; the position-pairing fix; #735)
- [x] 2026-10-19 — S9 transport-neutral review module (`reviewIpc` golden unchanged; import guard; `Transport.upload`; #731).
- [x] 2026-10-19 — B1a run identity in the review contract (review ABI 2, `review.run_identity`; track B step B1, not a slice).
- [ ] S10 attachable review bridge (ABI)
- [ ] S11 review commands in the studio command layer (ABI)
- [ ] S12 MIB Studio's options in the review contract (review ABI)
- [ ] S13 B5 mount
- [ ] S14 close-out
