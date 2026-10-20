# YOFO Review (React + Tauri)

> The standalone review product: opens the HDF5 files MIB Studio records on
> macOS and Windows with no camera, hardware or experiment code. Built from
> the same `desktop/` tree as the React + Tauri MIB Studio shell.

**Operator manual:** `docs/manual/yofo-review.md` (keep it in step with the
menus, exports and update flow below).

**Source:** `desktop/review.html`, `desktop/src/review/` (`main.tsx`,
`ReviewApp.tsx`, `ReviewPanel.tsx`, `RegenerateMasks.tsx`, `review.css`,
`charts/`, `exports/`, `open/`, `inspect/`, `shell/`),
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
page, selection, sub-view and set, the record card's image view) and the `"review"` / `"viewer"` slots of the
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
- **Record card + table** (`ReviewPanel.tsx`, `inspect/`) — the selected
  record's image (the full frame at full resolution with the backend
  overlay / ROI, or a crop, see R3 Inspect below) beside the paged records
  table (kit `DataTable`, 100 rows per page, `metricsColumns.ts`: every Qt
  column, default set, "Columns…" chooser persisted in `localStorage` key
  `yofo.review.columns`; recording files show index + timestamp only).
  Scatter, grid, table and record card share one selection; the table pages
  to follow it.
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
scatter on the left, an optional frame pane (`framePane`, omitted by R3
Inspect, whose record card sits beside the view) over the ring-width
histogram on the right; clicking a point selects that valid cell (same
selection as the Frames view, so the table page, record card and viewer
follow); ←/→ step through the valid set. Both charts are `<canvas>` with no
chart library; since #717 B3a they draw with the kit's colours
(`charts/chartPalette.ts`, the `tokens.css` values mirrored for the
offscreen exports: valid records in the accent colour, the inspected
record outlined in the inspect colour), on screen and in exported PNGs
alike (charts are compared with Qt through their data, `tools/review_parity`).

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
  publish: an existing output is refused, not replaced.
- **Regenerate gating:** YOFO enables Regenerate masks once a file is ready;
  Qt requires data.
- **Core fraction:** YOFO uses the stored record's fraction, else 0.9; Qt uses
  `Monitoring/KdeCoreFraction` (default 0.9).

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

Initializes the review bridge on boot (`init("")` → Tauri `app_data_dir`).
Since #717 B1 (#269) it is built on the `@mib/ui` kit (it imports
`@mib/ui/fonts.css` and `@mib/ui/style.css`; layout only in
`review/shell/shell.css`, every selector under `.rv-app`): an `AppShell`
(brand "YOFO Review", tag "Analysis · PC", context "Open run", status chips
for the backend and "Read-only · opening never modifies a file") with the
Review app's stages **Open** (R1), **Overview** (R2), **Inspect** and
**Export** (`REVIEW_STAGES`; Populations and Compare are reference views, not
scheduled). Header buttons replace the old menu row: Fit (the panel's image
scaling), Settings (Preferences), Updates…, About (stamped version from
`@tauri-apps/api/app`). The status bar and the log drawer stay below the
shell. **Preferences** holds the fallback px→µm (`localStorage`
`yofo.review.pixelToMicron`, default 0.4886 like the Qt tab) passed to the
panel. Selecting a stage only changes what is shown: no backend call
(`ReviewApp.test.tsx` checks it). No camera, experiment or hardware state
exists.

- **Open (R1, `review/open/OpenView.tsx`, `openModel.ts`, `libraryModel.ts`).**
  Open folder… (directory dialog) lists the folder with `review_list_dir`
  (`.h5` / `.hdf5` names only, hidden work files `.<name>.partial-…`
  dropped). Then it probes the files' headers with `review_probe_runs`
  (review ABI 2) in calls of at most `REVIEW_PROBE.max_paths` (256,
  `probeChunks`), one after another; the card shows "Reading N of M files"
  meanwhile. The folder is remembered (`yofo.review.libraryFolder`) and
  listed again at start. Open file…, Refresh and Close file sit beside it.
  - **Grouping (`groupRuns`, B1b part 2).** One group per declared
    `experiment_id`, newest first by its runs' recorded starts (experiments
    without one after, by id), runs by `run_index` (runs declaring none
    after, by name). Then, each by file name:
    - "Runs without an experiment": a snapshot (v1, an undeclared v3, or an
      unreadable one) without an `experiment_id`;
    - "Legacy files · no run snapshot";
    - "Cannot be read": `ok` false, the probe's per-file message on the row;
    - "Not read" / "Reading run identities…": no probe yet, or the probe was
      refused, with a warn `Notice` giving the backend's words; the rows stay
      openable.
  - **Tables.** The kit has no grouped table rows (#715), so each group is
    its own `DataTable`, its `caption` the group header (`GroupCaption`). An
    experiment's header carries runs · samples, start dates, conditions, core,
    calibration and one status chip (`groupSummary`: the worst state, "k of
    n runs · …" when mixed). Run rows show Run, File, Sample (`sample_ref` ·
    annotation sample), Condition, Started, Records, Core, Calibration, the
    accounting chip and Open. The "Runs without an experiment" group shows
    Snapshot in place of Run. Every table's missing value is "Not recorded".
    Experiment names, methods and plans are not in the files; a note says the
    workspace store holds them, and an experiment shows its id.
  - **Ordering guard (#747 review, extended to the probe).** The rows are
    kept with the folder they were listed for. A folder change drops the old
    rows at once. Each listing takes a generation, and both the names' answer
    and every probe answer of a superseded generation are dropped, so a late
    answer never fills a newer folder, nor a refresh's newer result. While
    the names are out, the card shows "Listing…" and the Open buttons are
    disabled. While the probe is out they stay enabled, since opening and
    probing are serialized by the review bridge. `openRun` also refuses a row
    from another folder or one shown during a listing. An unmount (#753
    review) supersedes the listing in flight: its cleanup bumps the
    generation and clears an `alive` ref, so a pending probe call's answer
    starts no further call, and a folder dialog answering afterwards lists
    nothing. A stage change does not unmount Open (the shell only hides
    it), so a probe goes on in the background and returning starts nothing
    new.
  - **Open run card.** Opening calls the panel's `openPath` (`review_open`),
    stays on Open and shows a `FactList` of what the file records: file,
    folder and kind; then `identityFacts(info.identity)` (Run snapshot,
    Experiment, Run, Sample, Condition, the rest of the annotation,
    Calibration record, the operator selected at Start; a legacy file says
    "Not recorded · legacy file" once and every other member is null); then
    records, started / ended, px→µm recorded or fallback, processing core and
    run accounting, with the accounting as one `StateChip`. A value the file
    does not record shows "Not reported", and a legacy file's accounting
    "Not recorded · legacy file", never Complete. A refused open is a blocked
    `Notice` with the backend's words, and nothing is open afterwards (the
    panel's `openPath` returns `{ ok, message }` and clears its info on
    failure: the backend has already closed the previous file).
- **Overview (R2):** a "not available yet" `Notice`: the statistics come
  from the #523 worker (step B2), and the shell shows no number it does not
  have.
- **Inspect (R3, #717 B3a):** `ReviewPanel` on the kit, see **R3 Inspect**
  below. It stays mounted (hidden) while another
  stage shows, so the open run, the selection and a running job survive a
  stage change. A launch file (`review_launch_path`, `?open=`) or a macOS
  open request (`review_take_open_request`) opens straight into Inspect.
- **Export (R6):** a `Notice` and a `FactList` naming where each export is on
  Inspect's toolbar, with "Go to Inspect"; the export view itself is step B4.

## R3 Inspect (#717 B3a)

`ReviewPanel` laid out as the Penpot R3 board (`dcdbaa6b-9790-4e69-bea4-936747f588b1`)
from the commands the review contract already has; nothing the file does not
record is shown as a value.

- **Toolbar** (kit `Toolbar`): the sub-views **Scatter** (experiment files:
  `ChartsView` in a `Card`, valid records only), **Records** (the table
  alone) and **Frames** (the thumbnail grid over the table; the default
  after an open, as before), then the set: **Valid Frames (n)** /
  **Invalid Frames (n)** (**Frames (n)** for a recording). A second row
  keeps the file actions and every export (Select HDF File…, Close File,
  Export Metrics to CSV…, Export All…, More…: Batch Metrics…, Batch Export
  All…, Export Charts…, Regenerate masks…), the overlay select (all five
  modes) and ROI, which apply to the thumbnails, the full frame and the
  viewer. A sub-view only changes what is shown and keeps the selection;
  the scatter from the invalid set moves to the valid set with nothing
  selected, and the invalid set from the scatter shows its frames.
- **Selected measurement** (`inspect/InspectCards.tsx` `RecordCard`): the
  record named `frame N · object M` (coordinator ruling on #717: files carry
  no record id and none is invented; the kit's `RecordIdentity` needs one,
  so the card uses its layout classes until #715 makes it optional), its
  place (`valid record i of n · run R · sample S`) and the run's declared
  provenance (experiment, snapshot version, calibration record, from the
  B1a identity). **Raw / Contour / Mask** ask `fetch_review_frame` for
  overlay None / AllContour / AllMask without the ROI and cut the square
  around the record's bounding box on the client (`cropSquare`: 1.6 × the
  box, at least 24 px, shifted inside the image; `inspect/packetCrop.ts`
  draws it through a canvas into a data URL for the kit's `CellImage`,
  which takes a URL). **Full frame** is the canvas the panel always drew
  (the toolbar's overlay and ROI; double-click opens the viewer). Views a
  record cannot show are disabled with the reason (a recording has no
  objects, a set without masks has no Contour / Mask); a crop that cannot
  be drawn says so, and a set without images shows the kit's "Image not
  recorded". The stored values (`measurementFacts`: area, deformability,
  stored E, ring ratio, timestamp, validation, track, objects in frame, the
  core) and **What is recorded?** (`recordedFacts`, from dataset presence:
  measurement, full frame, mask / contour, image series). **Previous /
  Next record** step through the set (its rows when it stores no images,
  #649) and **Record details** opens the frame viewer.
- **Stored vs reprocessed** (`StoredCard`): the core the stored results
  came from (at acquisition), the run snapshot, the calibration and px→µm
  (recorded or fallback, in the warn tone), the run accounting, and E-LUT
  **Not available** (the provenance is in the review contract from B3b,
  wired in B3c). "Reprocessing writes a new file; this view lists no
  history" (#754 defers regenerate provenance and Compare); **Reprocess
  into a new file…** opens the Regenerate masks dialog.
- **Records** (kit `DataTable`, compact): the selected row in the inspect
  colour (`aria-current`); a double-click on a row opens the viewer; ◀ / ▶
  page (the kit has no paging, #715).
- **Not shown** (rulings on #717): the analysis plan, populations, polygon
  and E gates, method names, Open in window.
- **Tests:** `inspect/inspectModel.test.ts` (names, crop square, view
  availability, facts) and `inspect/InspectView.test.tsx` (the panel over a
  mocked IPC: the overlay each view asks for and the crop rectangle, the
  reasons, stepping, the viewer from Record details / full frame / a row,
  sub-views keeping the selection, the Stored card and Reprocess, and every
  pre-B3a control still reachable). `reviewIpc.golden.json` is unchanged:
  the same commands and arguments flow as before (the test clicks
  **Scatter** where it clicked **Charts**).

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
