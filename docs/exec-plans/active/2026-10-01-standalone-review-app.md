# YOFO Review: the Review tab as its own React + Tauri application on macOS and Windows

Status: active

Date: 2026-10-01 (rewritten the same day: shell changed from Qt to
React + Tauri, product named **YOFO Review**). Companion notes:
`knowledge_map/architecture/Desktop-Shell.md`, `knowledge_map/architecture/Rust-Bridge.md`,
`knowledge_map/frontend/HdfReviewTab.md` (the behaviour to reproduce),
`docs/architecture/frontend-neutral-backend-bridge.md`, ADRs
[0001](../../decisions/0001-react-tauri-migration.md) (React + Tauri is the
target shell), [0003](../../decisions/0003-rust-cxx-bridge.md) (cxx bridge),
[0004](../../decisions/0004-bridge-contract-and-operation-state.md) (contract
governance); decoupling plan
[`2026-07-15-qt-decoupling-and-tauri-migration.md`](2026-07-15-qt-decoupling-and-tauri-migration.md);
review scatter plan
[`2026-09-30-review-scatter-click-to-view.md`](2026-09-30-review-scatter-click-to-view.md)
(its PR 3a and PR 4 are absorbed here); issues #269 (UI-4 HDF5 review),
#276 (BE-6 review jobs), #279 (BE-9 platform/updater), #465/#468/#470.

## Goal

A second shipped desktop product, **YOFO Review**, built on the React +
Tauri v2 shell and the Qt-free C++ backend, that opens the HDF5 files MIB
Studio records and offers everything today's Qt Review tab offers: browse
valid/invalid/recorded frames with thumbnails, the full metrics table,
overlay and ROI rendering, charts (scatter, histogram, isoelastic curves,
KDE colouring and core contour) with zoom/pan/click-to-view, single and
batch export through `HdfExportService`, chart export, regenerate masks,
compute/save core contour, recording-mode files, multi-image series and
run accounting. It ships for **macOS (Apple Silicon) and Windows x64** as
a signed, notarised DMG and a signed NSIS installer, updates itself on
both platforms, and is built and tested in CI on both. The binary
contains no camera, serial, discovery or experiment code. The same React
review module becomes MIB Studio's Review tab in the Tauri shell (UI-4),
so YOFO Review is also the delivery vehicle for that parity item.

## What the survey found (2026-10-01)

Why React + Tauri is the right base despite the gap:

- **The direction is already decided.** ADR 0001 names React + Tauri as the
  one supported shell once parity is reached; the Qt shell is
  transitional. Building YOFO Review on Qt would create a second product
  to migrate later.
- **The Qt-free backend already holds the science.** `Hdf5Service`,
  `HdfExportService`, `ProcessingService::processBatch`,
  `BatchMaskSources`, `KdeCoreRecord`, `MonitoringDensity` and
  `MonitoringDensityService` are all in `mib_processing`/`mib_backend`
  with no Qt. The Qt tab is 3 000 lines of widget code over them; what
  moves is presentation, not science.
- **The bridge, contract and shell scaffolding exist.** cxx bridge (ABI
  14), additive contract with a drift gate, binary frame-packet transport
  (`fetch_review_frame_packet`, pull kind 3), `FramePullScheduler` with a
  review slot, native file dialogs, preferences and log sinks
  (`platform.rs`), a fail-closed update-manifest verifier
  (`updater.rs`), headless Linux CI (`desktop-ci.yml`, Xvfb smoke).
- **Review already crosses the bridge, partially.** `load_recording`,
  `fetch_review_metadata`, `fetch_review_metrics_page`,
  `fetch_review_frame_packet` (datasets ValidImage/InvalidImage/
  RecordedImage/ValidMask/InvalidMask), `review_export_csv` as a tracked
  operation; `desktop/src/App.tsx:1753-1927` renders a Review panel with
  valid/invalid scrubbing and a 7-column paged metrics table.

What is missing or wrong today (evidence in the agent survey of
2026-10-01):

- **Review state lives in the wrong place.** `BackendFacade` serves review
  from `AppBackend::hdf5()`, the same handle the experiment writer uses
  (load refused while an experiment is active), and the Raw Frames view
  scrubs the **live FrameStore** through `PlaybackService`, not the file
  (`PlaybackService.cpp:30-45`). Multi-image series, run accounting, KDE
  records, chart snapshots, recorded config and the 16 other metric
  columns are not exposed. Export uses `review::writeMetricsCsv`, not
  `HdfExportService`; Export All, batch, chart export, regenerate masks
  and core contour are disabled placeholders (`App.tsx:1768-1770`).
- **No charts.** The Charts sub-tab is a placeholder; `desktop/package.json`
  has no chart library; nothing KDE-related crosses the bridge. The
  scatter plan already specifies the React Charts view (its PR 4) and the
  backend density job (PR 3a) with bridge calls `fetch_review_scatter`,
  `fetch_review_density`, `fetch_review_kde_records`,
  `review_save_core_contour`; none exist yet.
- **No thumbnails, no overlays, no ROI drawing, no Close File, no modal
  frame viewer.** Masks are addressable but never requested.
- **One product, one monolith.** `App.tsx` is a single 2 045-line component
  with camera/experiment/monitoring menus; `lib.rs` registers 81 commands;
  `tauri.conf.json` is "MIB Studio" with `bundle.active: false` and no
  `.icns`; there is no mode or product notion.
- **The bridge build has no macOS path.** `build.rs` treats every
  non-Windows host as Linux: runs the `linux-backend-only` preset (host
  condition Linux), hardcodes `/usr/include/opencv4` and
  `/usr/lib/x86_64-linux-gnu/hdf5/serial`, links `mib_backend` +
  `oeabt_*` + sentry + curl + sqlite. Windows links through a generated
  ~190-entry manifest from the Conan tree. `BackendBridge` always
  constructs a full `AppBackend` (cameras, serial, discovery, YOLO stub).
- **No packaging or updates.** Bundling is off, there is no `.icns`, the
  `updater.rs` verifier is not wired to any command, its manifest format
  (`url`, `sha256`) differs from the Qt one (`installer_url`,
  `installer_sha256`), no `tauri-plugin-updater`, no macOS or Windows
  Tauri CI job, no signing or notarisation anywhere in the repo.

## Decision log

- 2026-10-01: **Ship on React + Tauri; product name YOFO Review.** (User
  decision, superseding the same-day Qt proposal.) Record as **ADR 0008**:
  YOFO Review is the first product shipped on the Tauri shell and the
  reference for MIB Studio's Review tab (UI-4). The Qt tab keeps shipping
  in MIB Studio Qt until the Tauri cutover; it changes only where TD-17
  needs it.
- 2026-10-01: **One review implementation in the backend: `ReviewSession`.**
  A Qt-free class in a new static library `mib_review_core` (built from
  `mib_processing`, no `mib_backend`) owns the open file: its own
  `Hdf5Service` reader (never the experiment writer's handle), cached
  metadata, image/mask/series hyperslab reads, thumbnail strips, overlay
  composition, run accounting, KDE records, the isoelastic curves, the
  scatter/density job (`MonitoringDensityService` review mode from the
  scatter plan's PR 3a), export jobs over `HdfExportService`, mask
  regeneration over `ProcessingService::processBatch`, and core-contour
  compute/save. `BackendFacade` delegates every review pull and command
  to a `ReviewSession` it owns, which fixes the shared-handle and
  live-FrameStore defects for MIB Studio too. Shells never re-implement
  science; overlays are composed in the backend and returned as RGB in
  the frame packet so the Qt `OverlayRenderer` has one Qt-free
  counterpart.
- 2026-10-01: **Two bridge roots, one crate.** `crates/mib-bridge` gains a
  cargo feature `review-only` that exposes a `ReviewBridge` (over
  `ReviewSession` + one `ProcessingService`) and links only
  `mib_review_core` + `mib_processing` and their third-party libraries.
  Without the feature the crate is unchanged (`BackendBridge` over
  `AppBackend`), and `BackendBridge` also exposes the same review calls by
  delegation, so the contract is identical for both products. Rationale:
  the review binary must not carry cameras, serial, discovery, SQLite,
  curl or Sentry, and `AppBackend::initialize` always constructs them.
- 2026-10-01: **One Tauri crate, one React tree, two products by
  configuration.** `desktop/src-tauri` gains the cargo feature
  `review-only` (registers only the review, platform, dialog, opener and
  updater commands; sets `mib-bridge/review-only`) and a config overlay
  `desktop/src-tauri/tauri.review.conf.json` (`productName` "YOFO Review",
  `identifier` `bio.yofo.review`, `mainBinaryName` `yofo-review`, window
  title, icons, bundle targets, updater endpoints), applied with
  `tauri build --config tauri.review.conf.json --features review-only`.
  The React side gets a second Vite entry (`desktop/review.html` →
  `src/review/main.tsx`) that mounts only the review module; MIB Studio's
  `App.tsx` mounts the same module in its Review tab. A separate `review/`
  app directory was considered and rejected: it would fork `bridge.ts`,
  `framePacket.ts`, `eventAdapter.ts` and the scheduler within weeks.
- 2026-10-01: **Static third-party libraries for the review products.**
  OpenCV, HDF5, spdlog, fmt, nlohmann_json and OpenSSL are consumed as
  static Conan packages on macOS and Windows for `mib_review_core`, so the
  Tauri binary is self-contained and the bundle carries no dylib/DLL
  rpath work. Linux CI keeps system packages. The Windows link manifest
  generator (`tools/gen_bridge_link_manifest.py`) gains a `--review-only`
  mode over a `mib_review_core`-only reference target; macOS gets a
  CMake-written link manifest of the same shape instead of hardcoded
  paths, so `build.rs` reads one manifest format on both bundled
  platforms.
- 2026-10-01: **Charts on `<canvas>` with no chart library**, as the scatter
  plan's PR 4 specifies (10⁵ points, device-pixel-ratio aware, points per
  density level with the contract's `density_ramp`, contours as polylines,
  highlight last). Histogram and isoelastic curves draw on the same
  canvas module. Chart export renders the same drawing code to an
  offscreen canvas at a fixed size and sends the PNG bytes to the backend
  over the binary transport as the `HdfExportRequest` chart snapshots, so
  exported TIFFs show what the screen shows (the Qt tab's rule).
- 2026-10-01: **Updater: `tauri-plugin-updater` with minisign-signed
  `latest.json`**, served from the existing R2 bucket under
  `review-stable/` and `review-beta/`. It installs in place on macOS and
  Windows, which neither the Qt `AutoUpdater` (Windows only) nor the
  unwired `updater.rs` does. `publish-update.py --product review` writes
  the Tauri manifest shape (`platforms.darwin-aarch64`,
  `platforms.windows-x86_64`, `signature`, `url`); the existing SHA-256
  verifier stays as a second check on the downloaded bytes. MIB Studio's
  Qt manifests are untouched.
- 2026-10-01: **Separate product identity, shared version.** Identifier
  `bio.yofo.review`, binary `yofo-review`, app-config/app-data dirs under
  that identifier, preferences in `platform.rs`'s JSON document. The
  version is the repo's `PROJECT_VERSION` injected into the Tauri config
  at build time (`scripts/release/stamp-tauri-version.py` from
  `cmake/MIBVersion.cmake`), so one `vX.Y.Z` tag releases MIB Studio and
  YOFO Review together and a file recorded by X.Y opens in X.Y.
- 2026-10-01: **Pixel-to-micron factor is a YOFO Review preference, pending
  TD-17.** With no live backend the factor comes from the file's run
  snapshot when present, else the last-used preference; shown in the
  status bar. TD-17 (read the recorded factor for scatter and contour) is
  fixed in `ReviewSession` so both products benefit.
- 2026-10-01: **Regenerate masks uses the bundled kernel on both platforms
  in v1.** Signed plugin cores on macOS wait for published `.dylib` cores
  and the Apple signing story (OpenSSL Ed25519 verification is enabled on
  Apple in PR 5; a `macos`/`arm64` descriptor is generated; publishing
  cores is a follow-up).
- 2026-10-01: **macOS targets Apple Silicon only.** One Conan profile, one
  runner (`macos-14`), one DMG. Intel or universal is a later decision.
- 2026-10-01: **Signing is a user-side prerequisite.** Apple Developer ID +
  notarisation (`APPLE_CERTIFICATE`, `APPLE_ID`, `APPLE_PASSWORD`,
  `APPLE_TEAM_ID` as Tauri expects) and a publicly trusted Windows
  Authenticode certificate (the internal KPT CA used for the core DLL
  does not satisfy SmartScreen), plus the updater's minisign key pair
  (`TAURI_SIGNING_PRIVATE_KEY`). PR 7 is blocked until these exist as
  repository secrets; earlier PRs produce installable unsigned artefacts
  for internal testing.
- 2026-10-01: **No code signing or notarisation** (user decision: the app
  is not distributed through a store). Artefacts ship unsigned; the manual
  documents the one-time Gatekeeper step on macOS (right-click ▸ Open, or
  `xattr -d com.apple.quarantine`) and the SmartScreen "More info ▸ Run
  anyway" step on Windows. PR 7 is removed; the minisign key pair for
  `tauri-plugin-updater` is still generated (it is a self-issued key, not
  a certificate) so updates are verified against our own key.
- 2026-10-01 (PR 0): **The Tauri CLI's `--config` overlay is applied by
  setting `TAURI_CONFIG`** to the overlay's JSON for a plain `cargo build`,
  which is how `tauri-build` receives it from the CLI. The Linux lane
  builds the review context that way (no bundler on Linux) and checks the
  binary carries `bio.yofo.review`.
- 2026-10-01: **Headless end-to-end on Linux, native bundles on macOS and
  Windows.** The review module is driven under Xvfb with `tauri-driver`
  (WebDriver, Linux and Windows only) against fixture files; macOS CI
  builds, runs the cargo and vitest suites and the DMG smoke only. Parity
  with the Qt tab is checked on data, not pixels: `metrics.csv` and
  exported image TIFFs byte-for-byte, chart snapshots by point count,
  axis ranges and legend entries.

## Design

### Backend: `mib_review_core` and `ReviewSession`

```
include/backend/review/ReviewSession.h      src/backend/review/ReviewSession.cpp
include/backend/review/ReviewTypes.h        (metadata, metric rows (all 23 columns),
                                             thumbnail strip, overlay mode, series info,
                                             accounting, kde records, scatter, density)
include/backend/review/ReviewJobs.h         (export / batch / regenerate / core-contour
                                             operations with OperationStatus semantics, ADR 0004)
src/backend/review/OverlayCompose.cpp       (OpenCV port of frontend/utils/OverlayRenderer)
```

`ReviewSession` API (synchronous reads, bounded memory; jobs on their own
thread with cancel token, reporting through the existing operation-state
machinery):

- `open(path)`, `close()`, `metadata()`, `accounting()`, `kdeRecords()`,
  `runSnapshot()` (recorded pixel-to-micron, config), `isoelasticCurves()`.
- `metricsPage(dataset, offset, count)` with the full `ProcessedFrame`
  metric set the Qt `HdfMetricsModel` shows.
- `frame(dataset, index, overlayMode, roiOverlay)` → RGB8 composed image
  (+ raw Mono8 and mask on request); `seriesInfo(dataset, index)`,
  `seriesFrame(dataset, index, k, …)` for 4D datasets and the recording
  multi-image window.
- `thumbnails(dataset, offset, count, size)` → one packed Mono8 strip
  (count × size × size) so a 200-thumbnail page is one IPC pull.
- `scatter()` columnar valid-set arrays, `requestDensity()` /
  `density()` (the scatter plan's PR 3a, moved here), `saveCoreContour(overwrite)`.
- `startExport(HdfExportRequest)`, `startBatch(sources, root, metricsOnly)`,
  `startRegenerateMasks(request)` (bundled kernel, `processBatch`),
  `startComputeCore()`; all return an operation id; progress and terminal
  state through `OperationStatus`; partial outputs discarded on cancel.

`BackendFacade` keeps its public review methods but implements them by
delegating to a `ReviewSession` member; the `RecordingLoad` command opens
the session and no longer touches `AppBackend::hdf5()` or the FrameStore
for file frames (`RecordedImage` serves raw frames). Existing facade and
bridge tests keep passing; `record_then_load_and_review` gains an
assertion that the recorded frames come from the file.

### Bridge and contract

- Contract bump **14 → 15** (additive): `review_metric_columns` enum,
  `overlay_modes`, `review_datasets` extended with series kinds, pull kind
  `thumbnails` (5) and `chart_snapshot_upload` (push, 6) on the binary
  transport, commands `review_close`, `fetch_review_frame_packet(dataset,
  index, overlay, roi)`, `fetch_review_thumbnails`, `fetch_review_series_info`,
  `fetch_review_accounting`, `fetch_review_kde_records`,
  `fetch_review_scatter`, `fetch_review_density`, `fetch_review_run_snapshot`,
  `fetch_isoelastic_curves`, `review_export_all`, `review_export_charts`,
  `review_batch_export`, `review_regenerate_masks`, `review_compute_core`,
  `review_save_core_contour`; `operation_kinds` implements the reserved
  `BatchMetrics`, `MaskRegeneration` and adds `ReviewDensity`,
  `CoreContour`; `density_ramp` table. Regenerate `bridgeContract.ts`,
  `frame_packet_contract.rs`, extend `shim.cpp` asserts and `contract.rs`.
- `crates/mib-bridge` feature `review-only`: `src/review_bridge.rs` +
  `src/review_shim.{h,cpp}` expose `ReviewBridge` (`new_review_bridge(data_dir,
  pixel_to_micron)`, the calls above, `poll_events`); `BackendBridge`
  forwards the same calls to its facade. `build.rs` reads a link manifest
  on every platform (`build/<preset>/mib-bridge-link-manifest.json`,
  written by CMake for the chosen root target) and, with `review-only`,
  links `mib_review_core`, `mib_processing` and their static third-party
  libraries only.

### Shell: products by configuration

```
desktop/
  index.html                 MIB Studio entry (unchanged)
  review.html                YOFO Review entry → src/review/main.tsx
  src/review/                the review module, mounted by both entries
    ReviewApp.tsx            menu bar (File: Open…, Open Recent, Close, Export ▸, Quit;
                             Edit: Preferences…; Help: About, Check for updates…),
                             status bar (file, factor, accounting)
    FramesView.tsx           thumbnails grid (virtualised, 200 + 100 pages), metrics
                             table (virtualised, all columns, row ↔ thumbnail selection)
    FrameViewer.tsx          image + overlay/ROI controls, zoom, series prev/next,
                             modal "Open in window" overlay and the docked pane variant
    charts/                  ReviewScatter.tsx, Histogram.tsx, scatterGestures.ts,
                             scatterHitTest.ts (shared fixture), chartCanvas.ts,
                             chartExport.ts (offscreen render → PNG bytes)
    exports/                 ExportDialogs.tsx (series range prompt, batch summary),
                             operations.ts (progress/cancel over OperationStatus)
    RegenerateMasks.tsx      source picker (current file / whole file / AVI / folder),
                             ROI, background, progress
    preferences.ts           pixel-to-micron, last dirs, KDE toggle, splitter sizes
    *.test.ts(x)             vitest
  src-tauri/
    tauri.conf.json          MIB Studio (bundle on, nsis+dmg targets)
    tauri.review.conf.json   YOFO Review overlay
    icons/                   icon.icns added (both products)
    src/lib.rs               `#[cfg(feature = "review-only")]` command set;
                             updater plugin registered for both
    capabilities/default.json + capabilities/review.json
    Cargo.toml               feature review-only = ["mib-bridge/review-only"],
                             tauri-plugin-updater, tauri-plugin-process
```

### Platform build matrix

| | Linux (CI only) | macOS arm64 | Windows x64 |
|---|---|---|---|
| Backend archives | `linux-backend-only` (system packages) | new `macos-review-core` preset: Conan profile `macos-appleclang-arm64`, static deps, target `mib_review_core` | new `windows-review-core` preset (inherits `windows-ninja-ci`, static deps, `mib_review_core`) |
| Link into Rust | manifest written by CMake | manifest written by CMake | `gen_bridge_link_manifest.py --review-only` until the CMake writer replaces it |
| WebView | WebKitGTK | WKWebView (system) | WebView2 Evergreen (NSIS bootstrapper downloads it) |
| Bundle | none | `tauri build` → `.app` + `.dmg` | `tauri build` → NSIS `.exe` (per-user, no admin) |
| Sign | — | Developer ID + notarytool via Tauri env vars | signtool via `bundle.windows.signCommand` |
| Update | — | `tauri-plugin-updater`, `review-<channel>/latest.json` | same |
| CI job (`review-ci.yml`) | build + vitest + cargo tests + tauri-driver e2e under Xvfb | build + cargo/vitest + DMG mount smoke | build + cargo/vitest + tauri-driver e2e + installer smoke |

## Implementation plan

Nine PRs. PR 0 and PR 1 are structural and land first; PR 2, 3, 4 (UI) are
sequential on the review module; PR 5 (macOS) and PR 6 (Windows) depend on
PR 1 and can run in parallel with the UI PRs; PR 7 is blocked on secrets;
PR 8 closes documentation and parity. Every PR carries vault updates and
passes `python3 scripts/check_docs.py` and
`python3 scripts/gen_bridge_contract.py --check`.

### PR 0 — Product scaffolding, ADR 0008, bundling on

Files: new `docs/decisions/0008-yofo-review-on-react-tauri.md`,
`desktop/review.html`, `desktop/src/review/{main.tsx,ReviewApp.tsx}`
(renders today's Review panel extracted from `App.tsx:1753-1927` and
mounted in both entries), `desktop/vite.config.ts` (two inputs),
`desktop/package.json` (`build:review`, `test`), `desktop/src-tauri/Cargo.toml`
(feature `review-only`, `tauri-plugin-updater`, `tauri-plugin-process`),
`desktop/src-tauri/tauri.conf.json` (`bundle.active: true`, targets
`["nsis","dmg"]`, icns), new `desktop/src-tauri/tauri.review.conf.json`,
`desktop/src-tauri/icons/icon.icns`, `desktop/src-tauri/capabilities/review.json`,
`desktop/src-tauri/src/lib.rs` (cfg-gated handler list), new
`scripts/release/stamp-tauri-version.py`, new `.github/workflows/review-ci.yml`
(Linux job: build both entries, `cargo build --features review-only`,
cargo/vitest, Xvfb smoke of `yofo-review`), `desktop/scripts/xvfb-smoke.sh`
(binary name parameter already), vault: `Desktop-Shell.md`, new
`knowledge_map/frontend/YofoReview.md` (+ `_MOC.md`, `README`,
`Agent-Onboarding`), `Build.md`, `Run-Modes.md`, `Recent-Work.md`,
decoupling-plan parity matrix (new "YOFO Review" column), this plan.

Exit: `yofo-review` boots under Xvfb showing today's review panel with the
YOFO Review title; MIB Studio's shell unchanged; `tauri build` on Linux
produces a `.deb`/AppImage as proof the bundler runs (not shipped).

### PR 1 — `ReviewSession`, `mib_review_core`, review bridge, contract 15

Files: `src/backend/review/*`, `include/backend/review/*`,
`src/backend/CMakeLists.txt` (`mib_review_core` target; link-manifest
writer `cmake/MIBBridgeLinkManifest.cmake`), `cmake/MIBOptions.cmake`
(`MIB_BUILD_REVIEW_CORE_ONLY`), `CMakePresets.json` (`linux-review-core`),
`include/backend/app/BackendFacade.h` + `src/backend/app/BackendFacade.cpp`
(delegate), `include/backend/services/MonitoringDensityService.h` + `.cpp`
(review mode, from the scatter plan PR 3a), `include/backend/processing/MonitoringDensity.h`
(`densityAtPointsFromGrid`, `levelForDensity`), `crates/mib-bridge/{Cargo.toml,build.rs,src/lib.rs,src/shim.cpp,src/review_bridge.rs,src/review_shim.{h,cpp}}`,
`crates/mib-bridge/contract/bridge-contract.json` (ABI 15),
`crates/mib-bridge/tests/{contract.rs,review_contract.rs}`,
`scripts/gen_bridge_contract.py`, generated `desktop/src/bridgeContract.ts`
and `frame_packet_contract.rs`, `desktop/src-tauri/src/lib.rs` (review
commands over either bridge), `desktop/src/bridge.ts` (typed wrappers),
tests: new `tests/backend/review_session_test.cpp` (open/close, metadata,
pages equal `Hdf5Service::readValidMetadata` order, series reads, overlay
compose equals the Qt `OverlayRenderer` output on the fixture within one
grey level, thumbnails, accounting, KDE records, TD-17 recorded factor),
`tests/backend/review_jobs_test.cpp` (export equals `HdfExportService`
direct run byte-for-byte, batch continues after a failing file, cancel
discards partials, regenerate masks on the population fixture, compute
and save core record with overwrite/read-only refusals),
`tests/backend/review_density_test.cpp` (scatter plan PR 3a cases),
`tests/CMakeLists.txt` (labels `recording;review`, round-trip + fault
injection per the coverage matrix, TSan lane for the density service),
vault: new `knowledge_map/services/ReviewSession.md`,
`MonitoringDensityService.md`, `Rust-Bridge.md`, `HDF5-Storage.md` if
paths move, `HdfReviewTab.md` (TD-17 note), this plan.

Exit: `cargo test --features review-only` runs the review contract against
fixture files with no `mib_backend` in the link (`nm` check in CI);
MIB Studio facade tests green; `record_then_load_and_review` proves raw
frames come from the file.

### PR 2 — Frames view: open/close, thumbnails, full metrics table, viewer, overlays, series, recording files

Files: `desktop/src/review/{ReviewApp,FramesView,FrameViewer}.tsx`,
`desktop/src/review/thumbnails.ts` (strip decode + virtual paging, pure),
`desktop/src/review/metricsColumns.ts`, `desktop/src/review/preferences.ts`,
`desktop/src/framePullScheduler.ts` (thumbnail slot), `desktop/src/App.tsx`
(Review tab mounts the module; delete the old panel), vitest for the pure
modules, new `desktop/e2e/review_frames.spec.ts` (tauri-driver under Xvfb:
open fixture, thumbnails page in on scroll, click thumbnail → viewer →
prev/next, overlay switch changes pixels, recording fixture hides
Invalid and relabels Frames, Close clears), `review-ci.yml` (tauri-driver
step), vault `YofoReview.md`, `docs/manual/` draft page, this plan.

Behaviour to match: `HdfReviewTab.md` Responsibility, Recording-mode
files, Run accounting, Scalability (virtualised; > 2 GB files; never all
images at once), Gotchas (series via the series reader).

### PR 3 — Charts: scatter, histogram, isoelastic, KDE, contour, click-to-view pane

Files: `desktop/src/review/charts/*`, shared fixture
`tests/fixtures/review_scatter_hits.json` (already shared with the C++
test), `desktop/src/review/preferences.ts` (KDE toggle, pane split),
vitest (gesture state machine, hit-test fixture, level colouring falls
back while not ready, stale session ignored, histogram binning equals the
Qt `generateHistogram` on the fixture), e2e `review_charts.spec.ts` (click
a known point → pane shows the expected frame; drag pans; double-click on
empty space resets; 20 000-cell fixture opens under the gate), vault,
this plan; scatter plan progress rows for PR 3a/PR 4 marked done here.

Behaviour to match: scatter plan "Behaviour" and "Tauri parity" sections
(drag threshold 10 CSS px, left/middle pan, wheel with Ctrl/Shift and
axis regions, pixel hit test ≤ max(marker, 8), ties → lowest frame,
highlight drawn last, pane never covers the plot, Prev/Next and ←/→,
"Open in window" as an in-app overlay).

### PR 4 — Exports, regenerate masks, core contour, preferences

Files: `desktop/src/review/exports/*`, `desktop/src/review/charts/chartExport.ts`,
`desktop/src/review/RegenerateMasks.tsx`, `desktop/src/review/ReviewApp.tsx`
(menu wiring, status), `desktop/src-tauri/src/lib.rs` (chart snapshot
upload command on the binary transport), vitest (default naming
`<basename>_metrics.csv` with `_2`, `_3` suffixes, batch summary text,
series range parsing `9-15`), e2e `review_exports.spec.ts` (Export
Metrics equals the Qt tab's CSV byte-for-byte on the fixture; Export All
folder layout `<root>/<basename>/`; cancel mid-export leaves no partial;
Export Charts writes TIFFs; regenerate masks produces a file the viewer
reloads; compute core then save writes `/analysis @kde_core_json`),
`docs/manual/`, vault, this plan.

Behaviour to match: `HdfReviewTab.md` export rules (one job at a time,
progress + cancel, partial discarded, folders published on success, last
directories remembered, series prompt applies one range across all
records) and Regenerate masks section.

### PR 5 — macOS build chain and DMG (unsigned)

Files: `conan/profiles/macos-appleclang-arm64`, `conanfile.py` (static
options under `tools.build:…` for the review profile; no Qt requirement
when `MIB_BUILD_REVIEW_CORE_ONLY`), `CMakePresets.json`
(`macos-review-core`, build/test companions), `cmake/MIBDependencies.cmake`,
`src/backend/CMakeLists.txt:103,218-232` (OpenSSL on Apple; `macos`/`arm64`
core descriptor), `crates/mib-bridge/build.rs` (macOS manifest path),
`desktop/src-tauri/tauri.review.conf.json` (`bundle.macOS.minimumSystemVersion`
13.0, `fileAssociations` for `.h5`/`.hdf5`), `scripts/doctor.sh`,
`scripts/bootstrap.sh` (replace the "no preset yet" warning with the
Conan + preset steps), `env/brew-packages.txt` (rust, node), new
`docs/howto/macos-build.md`, `.github/workflows/review-ci.yml` (job
`macos-14`: Conan cache, preset build, `cargo test --features review-only`,
`npm test`, `tauri build --config tauri.review.conf.json --features review-only`,
mount the DMG and launch `yofo-review --version`), vault `Build.md`,
`Dependencies.md`, this plan.

### PR 6 — Windows installer, updater, R2 channels, release workflow

Files: `CMakePresets.json` (`windows-review-core`), `tools/gen_bridge_link_manifest.py`
(`--review-only`), `desktop/src-tauri/tauri.review.conf.json` (NSIS
per-user install, WebView2 bootstrapper, `.h5` association, `plugins.updater`
endpoints `https://updates.yofo.bio/review-stable/latest.json` and the
public minisign key), `desktop/src-tauri/src/lib.rs` (`check_for_updates`
command: plugin check → SHA-256 re-verify via `updater.rs` → install →
`tauri-plugin-process` relaunch), `desktop/src/review/ReviewApp.tsx`
("Check for updates…", channel in Preferences), `scripts/release/publish-update.py`
(`--product review --format tauri`: `latest.json` with `platforms`,
`index.json` history), `scripts/release/release.ps1` (build YOFO Review
too), `.github/workflows/review-ci.yml` (job `windows-2022`), `.github/workflows/release.yml`
(on `v*` tags: build and attach `YOFO_Review_vX.Y.Z_x64-setup.exe` and
`YOFO_Review_vX.Y.Z_aarch64.dmg`, publish both review channels),
`docs/howto/auto-update-r2.md`, `docs/howto/build-installer.md`, vault,
this plan.

### PR 7 — removed (no signing; decision log 2026-10-01)

Unsigned artefacts are the shipped form. The manual (PR 8) documents the
Gatekeeper and SmartScreen one-time steps. Only the updater's self-issued
minisign key is set up, in PR 6.

### PR 8 — Manual, screenshots, parity sign-off, hand-over

Files: new `docs/manual/yofo-review.md` (install on Mac and Windows, open
a file, every workflow of `review-and-postprocess.md` in YOFO Review
terms, Preferences, updates), `docs/manual/review-and-postprocess.md`
(cross-link), `mkdocs.yml`, a tauri-driver screenshot harness
(`desktop/e2e/screenshots.spec.ts`) registered in `scripts/check_screenshots.py`,
`docs/exec-plans/tech-debt-tracker.md` (TD-17 closed; TD-18 noted as not
applicable to the canvas scatter; new entries for anything deferred),
decoupling plan parity matrix (Review row → ships in both), scatter plan
→ completed, `Recent-Work.md`, this plan → `completed/`.

Parity sign-off: on the `z-adjustment-50v` conformance corpus and the
512x96 recorded run used by `integration.review_scatter_e2e`, YOFO Review
and the Qt tab produce identical `metrics.csv` and image TIFFs, the same
scatter point count and axis extents, the same histogram bins, the same
accounting text and the same saved core record; the differences list in
`YofoReview.md` is empty or every item is accepted in the decision log.

## Acceptance criteria

- [ ] `ReviewSession` in `mib_review_core` serves every review read and job
      for both products; `BackendFacade` delegates to it; MIB Studio's
      Tauri Review tab no longer reads the live FrameStore for file frames.
- [ ] Contract 15 is additive; `gen_bridge_contract.py --check`,
      `shim.cpp` asserts, `contract.rs` and the new `review_contract.rs`
      pass on Linux, macOS and Windows.
- [ ] `yofo-review` links no `mib_backend`, camera SDK, serial, SQLite,
      curl or Sentry symbols (CI `nm`/`dumpbin` check).
- [ ] Frames, viewer, overlays, ROI, series, thumbnails, full metrics
      table, recording files and accounting behave as `HdfReviewTab.md`
      specifies; e2e `review_frames` green on Linux and Windows.
- [ ] Charts: scatter with KDE colouring and contour, histogram,
      isoelastic curves, zoom/pan/click-to-view pane, shared hit fixture
      passes in vitest and C++; 20 000-cell fixture opens within the
      gate; the scatter plan's acceptance criteria for the React view hold.
- [ ] Exports (metrics, all, batch, charts), regenerate masks, compute and
      save core contour run as tracked operations with progress and
      cancel; outputs equal the Qt tab's byte-for-byte where the data is
      deterministic.
- [ ] macOS: `macos-review-core` + `tauri build` produce a DMG on
      `macos-14`; cargo and vitest suites pass there; `.h5` opens by
      double-click.
- [ ] Windows: NSIS installer installs per-user beside MIB Studio Qt
      without touching it; `.h5` association; "Check for updates…" installs
      a newer `review-stable/latest.json` release and relaunches.
- [ ] One `vX.Y.Z` tag publishes MIB Studio Qt and YOFO Review artefacts
      to GitHub Releases and their R2 channels.
- [ ] Unsigned artefacts install after the documented one-time Gatekeeper /
      SmartScreen step; the updater verifies its downloads against the
      project minisign key.
- [ ] ADR 0008 accepted; manual page, howtos, vault notes, screenshot
      harness and the decoupling-plan parity matrix updated;
      `check_docs.py` and `check_screenshots.py` clean.

## Non-goals (v1)

- Any change to the Qt Review tab beyond TD-17 (it keeps shipping in MIB
  Studio Qt until the Tauri cutover).
- Intel macOS or universal binaries; Linux installers (Linux is a CI
  target only).
- Crash reporting in YOFO Review (Sentry lives in `mib_backend`; a
  Qt-free, backend-free reporter is a follow-up under #279).
- Signed plugin processing cores on macOS (bundled kernel only).
- Multi-window sessions; live capture of any kind; the MIB Studio
  experiment, monitoring or hardware panels.
- A backend chart rasteriser (the shell renders charts; export sends its
  pixels).

## Progress

- [x] PR 0 — scaffolding: review entry, config overlay, `review-only`
      feature, bundling on, ADR 0008, `review-ci.yml` (Linux). Landed
      2026-10-01 on `plan/standalone-review-app`. Deviations from the PR 0
      file list: no `capabilities/review.json` (the review window keeps
      label `main`, so `capabilities/default.json` covers both products);
      `tauri-plugin-updater` / `tauri-plugin-process` wait for PR 6 (the
      updater plugin needs the minisign public key in the config at build
      time); bundling is on only in the review overlay (`dmg`, `nsis`),
      the MIB Studio config is unchanged.
- [x] PR 1a — `ReviewSession` / `mib_review_core`, facade delegation
      (landed 2026-10-01)
- [x] PR 1c — review bridge (`review_ffi`, feature `review-only`), contract
      15, Tauri `review.rs`, `reviewBridge.ts`, panel on the review bridge,
      CI `nm` check (landed 2026-10-01). Deviation: a **second bridge** over
      `ReviewSession` instead of new functions on `BackendBridge`, because
      cxx cannot cfg-gate functions inside one bridge module and the
      review-only binary must not compile the AppBackend shim; MIB Studio's
      Tauri shell holds both bridges and its Review tab uses only the
      review one.
- [x] PR 1b — review jobs (export, batch, regenerate masks, core contour)
      and the density review mode (landed 2026-10-01). Deviation: the
      density job lives in `ReviewJobs` (review core) rather than as a review
      mode of `MonitoringDensityService` (which is in `mib_backend`, which the
      review-only product must not link); it reuses the same
      `MonitoringDensity.h` kernel and bucketing.
- [x] PR 2 — Frames view, viewer, overlays, series, recording files
      (landed 2026-10-01). Verified by driving the review build on a
      real-cell file under Xvfb (screenshots), not yet by tauri-driver:
      the WebDriver e2e (`desktop/e2e/review_frames.spec.ts`) moves to PR 8
      with the screenshot harness, since `webkit2gtk-driver` is not in the
      CI image yet. Raw Frames tab dropped: recording files show their
      frames as the "Frames" set (the Qt behaviour).
- [x] PR 3 — Charts view, gestures, click-to-view pane (absorbs scatter
      plan PR 4 / #470; landed 2026-10-01). Verified by vitest (shared hit
      fixture, gestures, binning, maths), bridge tests on a new population
      fixture, and driving the review build under Xvfb on 3 000- and
      20 000-cell files (click/hover/zoom/pan/double-click reset, compute and
      save core contour); the tauri-driver `review_charts.spec.ts` moves to
      PR 8 with the other e2e specs. Deviations: no `preferences.ts` — the
      two toggles persist in `localStorage` (`yofo.review.charts`) and the
      pane split is a fixed 3:2 flex (resizable splitters go with PR 4's
      preferences); the histogram range is the file's recorded ring-ratio
      thresholds (Qt uses the live config); isoelastic curves are embedded
      in the binary (`include_str!`) instead of read from a bundle path.
- [ ] PR 4 — exports, regenerate masks, core contour, preferences
- [ ] PR 5 — macOS Conan profile, preset, bridge manifest, DMG, CI job
- [ ] PR 6 — Windows preset, NSIS, updater plugin, R2 review channels,
      release workflow
- [x] PR 7 — removed: no signing (user decision 2026-10-01)
- [ ] PR 8 — manual, screenshots, parity sign-off, tracker, plan → completed
