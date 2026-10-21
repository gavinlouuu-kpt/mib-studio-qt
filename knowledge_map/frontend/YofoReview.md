# YOFO Review (React + Tauri)

> The standalone review product: opens the HDF5 files MIB Studio records on
> macOS and Windows with no camera, hardware or experiment code. Built from
> the same `desktop/` tree as the React + Tauri MIB Studio shell.

**Operator manual:** `docs/manual/yofo-review.md` (keep it in step with the
menus, exports and update flow below).

**Source:** `desktop/review.html`, `desktop/src/review/` (`main.tsx`,
`ReviewApp.tsx`, `ReviewPanel.tsx`, `RegenerateMasks.tsx`, `review.css`,
`charts/`, `exports/`),
`desktop/src-tauri/tauri.review.conf.json`, cargo feature `review-only` in
`desktop/src-tauri/Cargo.toml` (features `studio` / `review-only`),
`src/main.rs` + `src/review_app.rs`, `src/review.rs`, `src/review_packet.rs`,
`crates/mib-bridge/contract/review-contract.json` (→ `src/review/reviewContract.ts`,
`src/review/reviewPacket.ts`), `desktop/scripts/tauri-review.mjs`,
`scripts/release/stamp-tauri-version.py`, `.github/workflows/review-ci.yml`
**Decision:** `docs/decisions/0014-yofo-review-on-react-tauri.md`
**Plan:** `docs/exec-plans/active/2026-10-01-standalone-review-app.md`
**Related:** [[../architecture/Desktop-Shell]], [[../architecture/Rust-Bridge]],
[[HdfReviewTab]] (the Qt behaviour being reproduced), [[../services/Hdf5Service]],
[[../services/HdfExportService]]

## How the two products share one tree

Since the develop merge (ADR 0014 amendment, decision A) MIB Studio's
Review tab is develop's own (#450 components and facade commands); the
review module below is YOFO Review's. #512 tracks bringing them back to one
implementation.

| Concern | MIB Studio (Tauri) | YOFO Review |
|---|---|---|
| Page | `index.html` → `src/main.tsx` → `App.tsx` | `review.html` → `src/review/main.tsx` → `ReviewApp.tsx` |
| Review UI | `App.tsx` with develop's review components (`ReviewCharts`, `SavedReviewImage`, export / reanalysis controls) | `ReviewApp.tsx` mounts `ReviewPanel` as the window |
| Tauri config | `tauri.conf.json` | `tauri.conf.json` + overlay `tauri.review.conf.json` (`--config`): product name, identifier `bio.yofo.review`, `mainBinaryName` `yofo-review`, window → `review.html`, bundle targets `dmg` + `nsis`, `.h5`/`.hdf5` association |
| Rust | the library (`lib.rs`, default feature `studio`, over `mib-app-commands`) | the binary built with `--no-default-features --features review-only`: `main.rs` → `review_app.rs` registers review, platform, isoelastic and update commands only; the library is empty |
| Contract | `bridge-contract.json` (MIB bridge ABI) | `review-contract.json` (`review_abi_version`), packets via `review_packet.rs` / `reviewPacket.ts` |
| Version | both stamped from `cmake/MIBVersion.cmake` by `stamp-tauri-version.py`; committed files carry the numeric `X.Y.Z` (CI `--check` compares that core — develop's `vX.Y.Z-beta.<sha>` tags cannot be committed), a pre-release suffix is stamped at build time | same |

Build locally:

```bash
cd desktop && npm install && npm run build             # both pages into dist/
cd src-tauri
TAURI_CONFIG="$(cat tauri.review.conf.json)" cargo build --no-default-features --features review-only
# or, with the bundler (macOS/Windows): npm run tauri:review:build -- --bundles dmg
# (scripts/tauri-review.mjs adds --config, --features review-only and the runner's
#  --no-default-features; tauri:review:dev the same for `tauri dev`)
```

`TAURI_CONFIG` is how the Tauri CLI passes `--config` overlays to
`tauri-build`; setting it by hand embeds the merged YOFO Review context in a
plain `cargo build`. Debug binaries load `devUrl`, so run `npm run dev` first
to see the UI, or build a release binary.

## `ReviewPanel` (shared module)

`src/review/ReviewPanel.tsx` owns the review state (file info, metrics
page, selection, sub-tab) and the `"review"` / `"viewer"` slots of the
host's `FramePullScheduler`, and imports `review.css` itself (the host page
needs no stylesheet). The host supplies: `ready` (backend initialized), the
scheduler, `fitWindow`, a `log` sink and three hooks — `beforeLoad` (MIB
Studio stops its live preview loop), `onFileChange` (host invalidates its
scheduler views), `onInfo` (workflow facts, status bar) and
`fallbackPixelToMicron` (TD-17 factor for files without a recorded one:
YOFO Review's preference, MIB Studio's applied processing factor; applied
with `review_set_pixel_to_micron` and the info re-read on change).
`ReviewPanelHandle` exposes `openFile` / `openPath` / `closeFile` for menus.
A file with no valid frames opens on its invalid set (`initialTab`).

The panel talks only to the **review bridge** (`src/review/reviewBridge.ts`
→ `src-tauri/src/review.rs` → `ReviewBridge` → [[../services/ReviewSession]]):
`open`/`close`, `info` (counts, ROI, series, accounting summary, the
recorded pixel-to-micron factor with a "(fallback)" marker — TD-17),
`rows` (full columns; the table shows index, object, track, area px²/µm²,
deformability, ring ratio, E), `frame(dataset, index, overlay, roi)` with the
overlay composed in the backend (Mono8 or RGB8 packets,
`packetToImageData`). Exports and mask regeneration are backend jobs
(`ReviewJobs`) — see **Exports and jobs** below.

**Run identity (review ABI 2, #717 B1a).** `info().identity` is the open
file's `ReviewRunIdentity` (run snapshot v3, #712, and the planned
annotation): `snapshot_version`, `run_id`, `experiment_id`, `run_index`,
`sample_ref`, `calibration_record_id`, `operator`, `annotation` (fixed
members + `free` key/values sorted by key). A member the file does not
record is `null`, never 0 or ""; a legacy file has `has_snapshot` false and
every member `null`; `snapshot_unreadable` / `annotation_unreadable` mark an
attribute that is not a JSON object. `reviewBridge.probeRuns(paths)`
(`review_probe_runs`) is the experiment library's header-only probe: at most
`REVIEW_PROBE.max_paths` (256, `reviewContract.ts`) paths per call, one
`ReviewRunProbe` per path in order (identity, start/end time, counts,
`accounting_completion`, `core_version`; `ok` false + `message` for a file
that cannot be read), refused as a whole while a review job runs. A count
the file does not record is `null` and a recorded 0 is `"0"`; a 0 time is
`null`. The B1a
PR adds the client only; R1 Open (B1b, on `feat/egrabber-workspace`) is its
first user. `reviewIpc.golden.json` records the probe call.

**Inspect commands (review ABI 3, #717 B3b).** `identity.emodulus_lut` is
the run's E-modulus LUT provenance (`lut_id`, `revision`, `source`,
`sha256`, `catalog_sha256`, `checksum_status`, each `null` when not
recorded; the whole member `null` when the snapshot records no loaded LUT)
and `identity.profile_id` the snapshot's profile. `reviewBridge.trackRows(valid,
trackId, max)` (`fetch_review_track_rows`) returns one track's records
(`positions` as decimal strings, parallel to `rows`; `total`, `truncated`;
`max` at most `REVIEW_TRACK_ROWS.max_rows` = 256), refused for an untracked
record (`track_id` -1). `reviewBridge.reprocessRecord(valid, index,
settings)` (`review_reprocess_record`, `settings` from
`REVIEW_REPROCESS.settings`: recorded 0, default 1) returns the stored row
next to the objects the session's kernel finds in the record's image now
(`reprocessed`, at most 64, `match_index` or `null`,
`object_count_differs`), with `settings_label`, `core_version` and
`core_source` naming what was used, `pixel_to_micron` the factor it ran
and reports with, and `emodulus_recomputed`. Since review ABI 4 (#764) E is
recomputed with the run's own E-LUT (the snapshot v3 record, found by its
hash in the loaded or the bundled LUT) and `emodulus_lut` /
`emodulus_lut_sha256` name it; both are `null`, `emodulus_recomputed` is
false and the reprocessed E is `null` when the run loaded no LUT or records
none (a file before snapshot v3). It never writes a file and is refused
while a review job runs, when the file's object coordinate space is
unknown, or when the run's recorded E-LUT has no hash or is not available
here. The UI on `feat/egrabber-workspace` (#774) still caveats the
reprocessed E; B3d can switch that caveat to the named LUT. `fetch_review_info` gains `object_coordinates` (`roi_local`,
`image` or `unknown`), `object_coordinates_source` (`attribute`,
`snapshot`, `mask_check`, `roi_origin`, or `""` when unknown) and
`object_origin_x` / `object_origin_y`, the offset that places a stored box
or centroid in image coordinates (#756 review 2; `REVIEW_OBJECT_COORDINATES`
in the generated contract). A client never places a box when the space is
`unknown`. The B3b PR adds the client only; R3 Inspect (B3a,
on `feat/egrabber-workspace`) is its first user. `reviewIpc.golden.json`
records the calls.

### Transport (#512 S9)

The module reaches the backend, the dialogs and the opener only through
`desktop/src/transport/` (see [[../architecture/Desktop-Shell]], "Frontend
transport"), like the operation app: `reviewBridge.ts` and `updates.tsx`
call `invoke`, chart staging calls `upload` (`reviewBridge.stageChart`), and
`ReviewPanel`, `RegenerateMasks` and `ChartsView` use `transport/dialogs`
(`open`, `save`, `ask`). In the desktop shells this is the same Tauri IPC as
before: `reviewIpc.test.tsx` drives the operator's flows (open, metrics
pages, frames, viewer and series, overlay / ROI, every export, charts and
the core contour, regenerate masks, Show in folder, close, update check) and
compares every call at `window.__TAURI_INTERNALS__.invoke` (command,
arguments, staged chart bytes and headers, dialog and opener options) with
`__snapshots__/reviewIpc.golden.json`, recorded before the move.
`transportBoundary.test.ts` fails when a non-test file under
`desktop/src/review/` imports `@tauri-apps/api/core`,
`@tauri-apps/plugin-dialog` or `@tauri-apps/plugin-opener`. `ReviewApp.tsx`
(the YOFO Review product shell) still uses `@tauri-apps/api/app`
(`getVersion`) and `@tauri-apps/api/event` (the macOS open-file event).

Over the WebSocket transport (YOFO Studio in a browser) the module behaves as
follows, pinned by `remoteTransport.test.tsx`:
- File dialogs become prompts for paths on the instrument (`transport/dialogs`).
- **Show in folder** is disabled with the reason "The output is on the
  instrument, so this browser cannot open its folder."
- Chart staging is refused by the transport (`TransportUnsupportedError`: the
  server reads no binary requests). The job dialog shows "Not started: Chart
  images cannot be sent to the instrument over this browser connection yet,
  so exports that include charts cannot run here."

The server does not serve the review commands yet, so the module does not run
remotely. Serving them is S11 of
[`2026-10-18-one-review-implementation.md`](../../docs/exec-plans/active/2026-10-18-one-review-implementation.md):
the review commands in `mib-app-commands` and a binary request path for
`review_stage_chart`. S10 is the bridge attaching to the facade's session.

**Until then the page says so (#763 F7).** `yofo-studio-server` answers each
review command with the string `UNKNOWN_COMMAND: <name>` (the dispatcher's
error, which `wsTransport` rejects with). Every `reviewBridge` call goes
through `gateReviewCommand` (`reviewAvailability.ts`): the first
UNKNOWN_COMMAND marks the review commands unavailable on that connection
(S11 registers them as one set), and later review calls are refused locally
with `ReviewUnavailableError` (no request, no server log); a reply marks them
available, which is all that happens in the Tauri shells. The answer holds
for one connection: the server may be restarted or upgraded with the page
open. `watchReviewConnection` (on `transport.session`) turns "unavailable"
into "checking" when the connection is lost or a new one has another client
id (a handover is not a new connection), and so does the notice's **Retry**.
While checking, `ReviewApp` probes `review_abi_version` again, retrying with
backoff (1 s doubling to 30 s) while the server cannot be reached; only a
reply enables the panel and the job poll again, and UNKNOWN_COMMAND leaves
them off with nothing more sent on that connection. The boot probe
(`review_abi_version`) decides it before any file is opened: `ReviewApp`
shows `REVIEW_UNAVAILABLE_TEXT` (role `alert`), passes `ready` false to the
panel (Open and the jobs stay disabled), skips the `?open=` / launch file and
the Tauri open-file listener (none in a browser; it threw `transformCallback`
as an uncaught page error), and logs "review commands are not served by this
server" once. `useReviewJobs` does not poll while they are unavailable, stops
at the first UNKNOWN_COMMAND answer with one log line, and logs any other
repeated poll error once instead of five times a second. Pinned by
`reviewAvailability.test.tsx` (a real `wsTransport` over a fake server that
knows only the boot commands, then restarts with or without them; Retry). Before this the page showed the backend as
ready, an empty folder, and the same error at 5 Hz. On the workspace branch
the R1 shell's "Backend ready" chip and the Open view's folder listing take
the same state (`useReviewAvailability`) after the develop sync.

## Frames view (PR 2)

The Qt tab's layout per set (Valid / Invalid, or "Frames" for a recording):

- **`ThumbnailGrid.tsx`** — virtualised: only rows in view (+2 overscan) are
  in the DOM; tiles come as packed strips (`fetch_review_thumbnails_packet`,
  pull kind 5) of `THUMB_PAGE` = 64 tiles × 128 px — 64 × 128 = 8192 is the
  frame packet's `max_dimension`, the bridge refuses taller strips. Pages
  live in a 12-page LRU (`PageCache`), invalidated on file / set / overlay /
  ROI change. Cells follow the image aspect (`imageBand`: a 512×96 frame is a
  128×24 band of the letterboxed tile; the Qt KeepAspectRatio rule). Arrow
  keys move the selection, Enter opens the viewer.
- **Preview + table** (`ReviewPanel.tsx`) — the selected frame at full
  resolution with the backend overlay / ROI over the paged metrics table
  (100 rows per page, `metricsColumns.ts`: every Qt column, default set,
  "Columns…" chooser persisted in `localStorage` key `yofo.review.columns`;
  recording files show index + timestamp only). Grid, table and preview
  share one selection; the table pages to follow it.
- **`FrameViewer.tsx`** — in-app overlay (scheduler slot "viewer"): frame
  prev/next (←/→, wraps), multi-image series (↑/↓; frame → series 1…n),
  zoom Fit / 1× / 2× / 4×, the frame's key metrics, Esc closes.
- **Launch file** — `review_launch_path` returns a pending OS open request,
  else the first existing `.h5`/`.hdf5` argument (`yofo-review run.h5`, the
  Windows/Linux file association); `?open=<path>` does the same for dev and
  the screenshot harness. macOS Finder opens arrive as `RunEvent::Opened`
  (`run()` in `review_app.rs`): the first HDF5 path is queued
  (`review::set_pending_open`) and `review-open-file` is emitted;
  `ReviewApp` takes it with `review_take_open_request` — taking means a
  cold-launch open read at boot is never opened twice. Windows/Linux start a
  second instance per double-click (no single-instance plugin).

Pure helpers are unit-tested in `src/review/review.test.ts`. Manual / visual
check: `cargo run --example review_fixture -- out.h5 --from-folder
build/vendor/assets/datasets/512x96stream-mock-frames` (in
`crates/mib-bridge`, `--features review-only`) regenerates a real-cell
experiment file through the review job, then launch the review build on it
under Xvfb with the dev server running.

## Charts view (PR 3)

Experiment files only (recording files carry no metrics). `charts/ChartsView.tsx`:
scatter on the left, the panel's preview pane over the ring-width histogram
on the right; clicking a point selects that valid cell (same selection as
the Frames view, so the table page, preview and viewer follow); ←/→ step
through the valid set. Both charts are `<canvas>` with no chart library.

- **Data** — `fetch_review_scatter` (columnar: area µm² with the recorded
  factor, deformability, valid-set position, frame index, ring ratio);
  `review_request_density` on open, then `fetch_review_density` polled
  (300 ms) until ready — per-point levels coloured on the contract ramp
  (`review_density.ramp_rgb`), grid path above 5000 cells; isoelastic
  curves from `fetch_isoelastic_curves` (`src-tauri/src/isoelastic.rs`,
  the `resources/isoelastic_curve` file embedded at compile time, so no
  bundle path can be missing).
- **Contours** — stored full-run record (solid `#2a78d6`), live
  provisional record (dashed `#eb6834`), and an unsaved full-run record:
  the density job's (files with none stored) or one computed from the
  context menu. "Save computed core contour to file" writes it
  (`review_save_core_record`, asks before replacing a stored one) and
  re-reads the info.
- **Gestures** (`scatterGestures.ts`, the Qt `ZoomableChartView`): wheel
  zooms ~10 %/notch about the pointer — Ctrl x only, Shift y only, over the
  y labels y only, under the plot x only; left/middle drag pans after
  10 px (Manhattan); a click selects the nearest visible point within 8 px,
  ties to the lowest frame (`scatterHitTest.ts`, checked against
  `tests/fixtures/review_scatter_hits.json` like the Qt test); double-click
  resets the zoom only when neither click hit a point; right-click menu:
  Reset zoom, Compute core contour from full run, Save computed core
  contour, Colour by density, Isoelastic curves (toggles persisted in
  `localStorage` key `yofo.review.charts`).
- **Histogram** — the Qt `generateHistogram`: isValid cells with
  ringRatio > 0, values clamped, 0.5-wide bins, y to ceil(1.1 × max); the
  x range is the file's recorded `ring_ratio_min..max` (`has_recorded_config`;
  defaults 15–25 otherwise) where Qt uses the live config.

Pure maths (`chartMath.ts`) and the gesture / hit-test modules are covered
by `charts/charts.test.ts`. Visual check: `cargo run --example
review_fixture -- out.h5 --population 3000` writes two seeded populations
with no stored record (density + computed contour + histogram); 20 000
cells render with density ready in ~2–3 s.

## Exports and jobs (PR 4)

- **Job tracking** (`exports/useReviewJobs.ts`): the panel owns the review
  event drain (`poll_review_events` at 5 Hz), logs every
  terminal state and tracks the one job it started. Events are buffered per
  operation id, so a job that finishes before its start call returns still
  reaches its dialog. An optional `prepare` step (chart rendering) runs
  first behind a "Preparing…" state.
- **`JobDialog`** (`exports/ExportDialogs.tsx`): progress bar + phase text
  (paths shortened to file names), Cancel; then the outcome — "Written to
  …" with **Show in folder** (`revealItemInDir`; disabled with its reason
  in a browser), the batch summary with
  per-file failures, "Cancelled. Partial output was discarded", or the
  refusal ("Not started: Another review job is still running"). One job of
  any kind runs at a time: the bridge's lane policy
  (`ReviewJobsOptions::forReviewBridge()`, #512 S7) keeps YOFO Review off the
  second lane (an export beside a regenerate / density / core job) until
  HDF5 is thread-safe (#722).
- **Toolbar**: Export Metrics, Export All, and a **More…** menu with Batch
  Metrics, Batch Export All, Export Charts, Regenerate masks (the Qt
  layout); job buttons disable while a job runs.
- **Rules** (`exports/exportHelpers.ts`, Qt parity): metrics default to
  `<basename>_metrics.csv`, then `_N` one past the highest suffix in the
  directory (`HdfExportService::nextAvailableName`, mirrored; directory
  listed through `review_list_dir`). Since #735 the proposal is
  conservative: a name counts as taken when any existing name matches it
  ignoring case (Windows and macOS volumes), suffixes above 999 999 999 are
  ignored (one such as 2^53 cannot be stepped in JS), and the search is
  bounded. Past the bound it falls back to `<prefix>YYYYMMDD-HHMMSS<suffix>`.
  The last successful export directory
  is remembered (`localStorage` `yofo.review.lastExportDir`, else the
  file's directory); Export All asks the series question (all / 1-based
  range such as `9-15` / skip) when records have series, Batch Export All
  always — one choice for every record.
- **Chart snapshots** (`charts/chartExport.ts`): `drawScatter` /
  `drawHistogram` (the on-screen drawing, shared) render offscreen at
  1200 × 1200 over the whole run (data extent, no selection ring — the Qt
  rule) with the Charts view's toggles and contours (persisted defaults
  when it was never opened); density colours only when a ready result for
  this file exists. PNG bytes are staged one raw-body request each
  (`reviewBridge.stageChart` → the transport's `upload` →
  `review_stage_chart`, header `x-chart-name`) and consumed by the next
  `review_export_all` / `review_export_charts`; the backend writes TIFFs.
  Recording files export no charts. Over the WebSocket the upload is refused
  (see **Transport** above).
- **Regenerate masks** (`RegenerateMasks.tsx`): source (whole file, valid /
  invalid range with 1-based start + count, AVI, folder), recorded config
  / ROI / background by default (AVI and folder use defaults), synthesize
  background; the panel opens the result (the Qt tab reloads from it).
  Since #512 S8 the job runs on the shared reanalysis engine and **never
  replaces a file** (partial file + no-replace publish, D6). **Save as…**
  lists the output directory (`review_list_dir`) and proposes a free name,
  `<base>_remasked.h5` then `_N` one past the highest suffix
  (`defaultRegenerateName`, with the rules above). If the operator picks an existing file anyway
  (past the OS dialog's own overwrite question), the backend refuses the
  start ("Output already exists; choose a new file"). The dialog stays open
  and shows "Not started: <reason> (<file>)", clears the output and the
  operator chooses again. The dialog closes once the job has started:
  `useReviewJobs.start` takes `{onRefused, onStarted}` hooks, so a refusal goes
  back to the dialog instead of the job dialog. An output created while the
  job runs fails the job and is left untouched.
  **Pixel-to-micron factor (#758):** with the recorded settings objects are
  validated (area range, target group) at the file's recorded factor, or the
  fallback preference when it records none; with default settings and for AVI
  / folder sources at 0.4886 µm/px. The output records the factor, so it
  reopens at it (no "(fallback)"), and the job message ends with
  "px→µm F recorded | default | fallback: the file records none". The
  recorded target group comes from the file's config document. Before #758
  every regenerate validated at 0.4886 and, with recorded settings, selected
  no target group; the pending release notice says which files to regenerate.
  Since #764 a factor that is not the source's (the fallback, or 0.4886 with
  default settings) is recorded with `@pixel_to_micron_source = "fallback"`,
  so the regenerated file reopens with "(fallback)", not as recorded.
  **E-LUT (#764):** E is computed with the source run's own E-LUT (snapshot
  v3, found by its hash in the bundled LUT, since YOFO Review loads none),
  and the message adds "; E-LUT <name> (the source run's)". A run that loaded
  none gets no E ("E not computed: the source run loaded no E-LUT"); a file
  that records none keeps the old behaviour (no E in YOFO Review: "E not
  computed: the source records no E-LUT and none is loaded"). A run whose
  LUT is not the bundled one fails with the reason, before writing. The new
  file records what E was computed with (`@emodulus_lut_json`: the run's
  record carried over, or the host's LUT); with the run's record,
  regenerating or reprocessing it again uses the same LUT, or none, while a
  host record is legacy, said as such.

## Differences from the Qt tab (parity sign-off, 2026-10-04)

Measured by `tools/review_parity/` (README there) on the `z-adjustment-50v`
corpus, the real-cell 512x96 run of `integration.review_scatter_e2e`, and two
`review_fixture` files recorded at 0.25 µm/px. Across those four inputs, the
following are identical: metrics.csv; Export All images, including series;
scatter point count and axis extents; histogram bins and labels; the saved
core record apart from `computed_at_ns`; the core status text; and the
accounting text for recording files.

**Fixed on 2026-10-04 to match (the Qt behaviour, or TD-17 for both):**
- Core record `cell_count` is the in-core count (the job used to store every
  valid cell).
- The fallback px→µm defaults to 0.4886 (it was 1.0).
- Batch export uses each source's recorded factor.
- Batch Metrics refuses recording files.
- The Qt tab reads the recorded factor (TD-17).

**Accepted (decision log of the plan):**
- **Ranges and axes:**
  - The histogram range comes from the file's recorded thresholds; Qt uses the live config.
  - The histogram y axis is `ceil(1.1 × max)`; Qt applies `applyNiceNumbers()`.
  - Non-finite ring ratios are dropped before binning.
  - The degenerate-extent fallback (all points equal) is ±max(1, 10 %) / ±max(0.01, 10 %).
- **Charts:**
  - Chart pixels and legend order differ: YOFO draws contours before isoelastic curves.
  - YOFO adds a "Core N% (full run, not saved)" family.
  - Exported charts use density colours when a result is ready.
- **Exports:**
  - Batch Export All writes no chart TIFFs per file.
  - Batch default names take one past the highest `_N`; Qt fills the first gap.
  - Batch series is one choice for all files; Qt prompts per file.
  - Export All asks the series question before the folder; Qt asks after it.
- **Status text:** for experiment files, the Qt status line replaces the
  accounting suffix with "Valid: N, Invalid: M" (metadata counts). YOFO shows
  the accounting in the file label, with counts from the experiment info.
- **Regenerate masks:** YOFO uses the recorded config and a median-of-32
  background. Qt uses the live config, a tiled synthetic background and
  timestamp order. Since #512 S8 these are options of the one reanalysis
  engine (`backend/review/Reanalysis.h`, decision D6). YOFO runs
  `timestamps = Rebase` (minus the first selected frame, clamped at 0),
  `syntheticBackgroundKind = MedianOf32` and `frameOrder = Datasets` (the whole
  file as valid rows then invalid rows). It also sets `convertColorToGray`, so
  colour HDF frames become gray as `loadFromHdf5` made them. The facade (MIB
  Studio's reanalysis) keeps Preserve / QuietTile / Timestamp and refuses
  colour frames. The content is unchanged on the S8 characterization fixtures,
  with one fix: when tracking merges frames into one record, each record now
  carries its own frame's index, timestamp and track frames. The pre-S8 job
  paired records with frames by position, so on the real-cell 512x96 run 1040
  of 1049 records named another frame. Files regenerated with YOFO Review up to
  review-v1.1.3-beta.1 carry that mis-paired per-record metadata; the
  data-integrity follow-up is #737. YOFO also gained the facade's no-clobber
  publish: an existing output is refused, not replaced. Since #758 all three
  validate at the source's own factor (an HDF source's recorded one; YOFO's
  fallback preference or default, the facade's and Qt's live factor
  otherwise), passed per call, never set on the shared kernel.
- **Regenerate gating:** YOFO enables Regenerate masks once a file is ready;
  Qt requires data.
- **Core fraction:** YOFO uses the stored record's fraction, else 0.9; Qt uses
  `Monitoring/KdeCoreFraction` (default 0.9).

### MIB Studio against Qt and YOFO (#512 S6, 2026-10-20)

`tools/review_parity/` has a third half since #512 S6:
`crates/mib-bridge/examples/review_parity_studio.rs` drives MIB Studio's
facade commands over `BackendBridge`:

- `load_recording`;
- `fetch_review_charts_json`;
- `review_export_json` metrics_csv / all, with `exportControls.tsx`'s defaults;
- `render_review_overlay`.

YOFO's half also dumps `fetch_review_frame` overlays (`yofo/overlays/`) and
the recorded ROI for the overlay comparison. The studio half charts each file
three times:

- as opened (the live 0.4886 and 15–25);
- with the live ring range set to the recorded one ± 5 (the D4 probe);
- with the live factor and range set to the recorded ones, as the Qt dump does.
  This pass produces the compared outputs.

Every acceptance is checked by its effect (README there). Each studio table
also lists every dump file and summary key its checks read (*expected
artefacts*) and the studio run's failures and export states (*studio run*);
an absent one is a DIFF, never a skipped check.

The run of 2026-10-20 used develop `1826a00b` + S6 and the four sign-off
inputs. `run.sh` exits 0. Outputs are in
`/mnt/hdd/developer-data/mib-studio-qt/review-parity/out-2026-10-20-s6-r3/`.
These are identical to Qt and YOFO:

- metrics.csv;
- Export All, apart from the chart TIFFs (30878 / 587 / 45 / 3301 files);
- scatter point count;
- scatter axis extents. On `population_3000`, YOFO's double padding is 1 ulp
  off the shared kernel's long double; the 1e-12 tolerance absorbs it.
- histogram bins and range.

Identical to YOFO: the density cells and the 30 ROI-off overlay renders per
experiment file. Only a point within 1e-9 of a cell edge may sit in the cell
across that edge; every other point must be in its own cell. No point of the
sign-off inputs is that close to an edge.

Accepted until S13 (plan decisions):

- **D2:** the ROI-on overlays differ only where each half painted exactly its
  own style's band at the recorded ROI, every band pixel in its colour.
  MIB Studio draws yellow at thickness 1; YOFO draws red at thickness 3, which
  OpenCV paints as a 5 px band with rounded outer corners (`compare.roi_band`,
  checked against `cv2.rectangle` goldens).
- **D3:** the as-opened px→µm on the two 0.25 µm/px fixtures (live 0.4886), with
  the area extents scaled by (0.4886 / 0.25)².
- **D4:** the probe's bins over [10, 30]. As opened, D4 is not exercised: every
  input records 15–25, the live default.
- **D5:** Export All carries `scatter_plot.tiff` and `histogram.tiff`.
- **D10:** the histogram y axis. This is the coordinator's ruling of 2026-10-20
  (named D6 in the ruling; the plan's D6 is the regenerate semantics).
  MIB Studio's inline tab tops it at the largest bin
  (`components/ReviewCharts.tsx` `maxBin`: 64 on the 512x96 run). Qt applies
  `applyNiceNumbers()` (80), and YOFO uses `ceil(1.1 × max)` (71). This is
  display headroom only; the bins are identical.

The Qt ↔ YOFO table is unchanged from 2026-10-04, except for the accounting
text. Qt's status line now carries the accounting suffix too, so that row
passes instead of being an accepted difference.

## Updates (PR 6)

`src-tauri/src/review_update.rs` (review-only feature, `tauri-plugin-updater`
2.x): `review_check_update(channel)` / `review_install_update(channel)`
against `https://updates.yofo.bio/review-<stable|beta>/latest.json`; the
plugin verifies the minisign signature against
`tauri.review.conf.json` `plugins.updater.pubkey`, then `verify_sha256`
checks the platform entry's `sha256` (fail closed), installs and restarts.
The plugin is registered only when that public key is present — until the
key is committed the commands answer `configured: false`.
`src/review/updates.tsx`: Help ▸ Check for updates… dialog, the channel in
Preferences (`localStorage` `yofo.review.updateChannel`), and one quiet
check per launch that puts "Update X available" in the status bar.
Publishing: `scripts/release/publish-review-update.py` (Tauri `latest.json`
+ `sha256`, artifacts first, manifest last), run by `review-release.yml`
when the bundles carry signatures. `review-release.yml` runs on the shared
`vX.Y.Z` tags and on YOFO-Review-only `review-vX.Y.Z[-beta.N]` tags, which
`release.yml` ignores and which get their own GitHub Release. Setup: `docs/howto/auto-update-r2.md`
("YOFO Review").

## `ReviewApp` (the product shell)

Initializes the review bridge on boot (`init("")` → Tauri `app_data_dir`)
and renders the menu row (File ▸ Open… / Close / Preferences…, View ▸ Fit,
Help ▸ About with the stamped version from `@tauri-apps/api/app`), the
panel, a status bar and the log drawer. **Preferences** holds the fallback
px→µm (`localStorage` `yofo.review.pixelToMicron`, default 0.4886 like the Qt tab) passed to
the panel. No camera, experiment or hardware state exists. Against a server
without the review commands it shows the unavailable notice (with Retry)
instead of a usable Open (see **Transport**, #763 F7).

## CI

`review-ci.yml` (Linux, headless): version-stamp check, backend archives,
contract drift gate, both pages built (`dist/review.html` must exist),
vitest, `cargo build --features review-only` under `TAURI_CONFIG`, a
`strings` check that the binary carries `bio.yofo.review`, an `nm` check
that it links no `backend::AppBackend` (and does link `ReviewSession`),
`cargo test --features review-only` in `crates/mib-bridge`, the link-manifest
generator tests, and the Xvfb boot smoke.

`review-macos` (macos-14) and `review-windows` (windows-2022) — the reusable
`review-bundles.yml`, called by `review-ci.yml` and, on `v*` tags, by
`review-release.yml` (version stamped from the tag; DMG + installer +
`SHA256SUMS-yofo-review.txt` appended to the GitHub Release) — build what
ships: Conan `review_core=True` (static spdlog / HDF5 / OpenCV core, imgproc,
imgcodecs, videoio — no Qt, no FFmpeg; `~/.conan2/p` cached), the
`<os>-review-core` preset, the probe run plus an `otool -L` / `dumpbin
/dependents` check that nothing third-party is loaded dynamically, the link
manifest, `cargo test --features review-only`, then `npm run tauri:review:build --
--features review-only` → `YOFO Review.app` + DMG (ad-hoc signature,
`LSMinimumSystemVersion` 13.0, mounted and checked) / per-user NSIS
installer (silent `/S` install under `%LOCALAPPDATA%\YOFO Review`), a
20-second smoke launch, and the DMG / installer uploaded as artifacts. No
signing or notarisation. How-tos: `docs/howto/macos-build.md`,
`docs/howto/build-installer.md` (YOFO Review section).

## Gotchas

- The overlay replaces the whole `app.windows` array (JSON merge patch), so
  the review window must restate `label: "main"` — `capabilities/default.json`
  targets that label for both products.
- The cargo binary is still named `mib-studio-desktop` in `target/`;
  `mainBinaryName` applies when the Tauri CLI bundles.
- Keep product identity in the config overlay and the feature flag only;
  no `#ifdef`-style branching in React sources (ADR 0014).

Metric labels and frame-viewer summaries use the shared `metricFormat.ts` finite
number formatter (em dash for absent/non-finite values). The shell wraps its main
review panel in `PanelErrorBoundary` with a Reload view action (#540).
