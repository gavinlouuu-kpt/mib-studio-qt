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
  …" with **Show in folder** (`revealItemInDir`), the batch summary with
  per-file failures, "Cancelled. Partial output was discarded", or the
  refusal ("Not started: Another review job is still running").
- **Toolbar**: Export Metrics, Export All, and a **More…** menu with Batch
  Metrics, Batch Export All, Export Charts, Regenerate masks (the Qt
  layout); job buttons disable while a job runs.
- **Rules** (`exports/exportHelpers.ts`, Qt parity): metrics default to
  `<basename>_metrics.csv`, then `_N` one past the highest suffix in the
  directory (`HdfExportService::nextAvailableName`, mirrored; directory
  listed through `review_list_dir`); the last successful export directory
  is remembered (`localStorage` `yofo.review.lastExportDir`, else the
  file's directory); Export All asks the series question (all / 1-based
  range such as `9-15` / skip) when records have series, Batch Export All
  always — one choice for every record.
- **Chart snapshots** (`charts/chartExport.ts`): `drawScatter` /
  `drawHistogram` (the on-screen drawing, shared) render offscreen at
  1200 × 1200 over the whole run (data extent, no selection ring — the Qt
  rule) with the Charts view's toggles and contours (persisted defaults
  when it was never opened); density colours only when a ready result for
  this file exists. PNG bytes are staged one raw IPC call each
  (`review_stage_chart`, header `x-chart-name`) and consumed by the next
  `review_export_all` / `review_export_charts`; the backend writes TIFFs.
  Recording files export no charts.
- **Regenerate masks** (`RegenerateMasks.tsx`): source (whole file, valid /
  invalid range with 1-based start + count, AVI, folder), recorded config
  / ROI / background by default (AVI and folder use defaults), synthesize
  background, output `<basename>_remasked.h5`; the panel opens the result
  (the Qt tab reloads from it).

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
  timestamp order.
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

Initializes the review bridge on boot (`init("")` → Tauri `app_data_dir`)
and renders the menu row (File ▸ Open… / Close / Preferences…, View ▸ Fit,
Help ▸ About with the stamped version from `@tauri-apps/api/app`), the
panel, a status bar and the log drawer. **Preferences** holds the fallback
px→µm (`localStorage` `yofo.review.pixelToMicron`, default 0.4886 like the Qt tab) passed to
the panel. No camera, experiment or hardware state exists.

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
