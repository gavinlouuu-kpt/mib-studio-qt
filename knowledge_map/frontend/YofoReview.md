# YOFO Review (React + Tauri)

> The standalone review product: opens the HDF5 files MIB Studio records on
> macOS and Windows with no camera, hardware or experiment code. Built from
> the same `desktop/` tree as the React + Tauri MIB Studio shell.

**Source:** `desktop/review.html`, `desktop/src/review/` (`main.tsx`,
`ReviewApp.tsx`, `ReviewPanel.tsx`, `review.css`, `charts/`),
`desktop/src-tauri/tauri.review.conf.json`, cargo feature `review-only` in
`desktop/src-tauri/Cargo.toml` + `src/lib.rs`,
`scripts/release/stamp-tauri-version.py`, `.github/workflows/review-ci.yml`
**Decision:** `docs/decisions/0008-yofo-review-on-react-tauri.md`
**Plan:** `docs/exec-plans/active/2026-10-01-standalone-review-app.md`
**Related:** [[../architecture/Desktop-Shell]], [[../architecture/Rust-Bridge]],
[[HdfReviewTab]] (the Qt behaviour being reproduced), [[../services/Hdf5Service]],
[[../services/HdfExportService]]

## How the two products share one tree

| Concern | MIB Studio (Tauri) | YOFO Review |
|---|---|---|
| Page | `index.html` → `src/main.tsx` → `App.tsx` | `review.html` → `src/review/main.tsx` → `ReviewApp.tsx` |
| Review UI | `App.tsx` mounts `ReviewPanel` in its Review tab | `ReviewApp.tsx` mounts `ReviewPanel` as the window |
| Tauri config | `tauri.conf.json` | `tauri.conf.json` + overlay `tauri.review.conf.json` (`--config`): product name, identifier `bio.yofo.review`, `mainBinaryName` `yofo-review`, window → `review.html`, bundle targets `dmg` + `nsis`, `.h5`/`.hdf5` association |
| Commands | full `generate_handler!` list | `--features review-only`: review, platform, dialog, operation control only (`invoke_handler()` in `lib.rs`) |
| Version | both stamped from `cmake/MIBVersion.cmake` by `stamp-tauri-version.py` (CI checks `--check`) | same |

Build locally:

```bash
cd desktop && npm install && npm run build             # both pages into dist/
cd src-tauri
TAURI_CONFIG="$(cat tauri.review.conf.json)" cargo build --features review-only
# or, with the bundler (macOS/Windows): npm run tauri:review -- build -- --features review-only
```

`TAURI_CONFIG` is how the Tauri CLI passes `--config` overlays to
`tauri-build`; setting it by hand embeds the merged YOFO Review context in a
plain `cargo build`. Debug binaries load `devUrl`, so run `npm run dev` first
to see the UI, or build a release binary.

## `ReviewPanel` (shared module)

`src/review/ReviewPanel.tsx` owns the review state (file info, metrics
page, selection, sub-tab) and the `"review"` / `"viewer"` slots of the
host's `FramePullScheduler`, and imports `review.css` itself (both products
get the styles). The host supplies: `ready` (backend initialized), the
scheduler, `fitWindow`, a `log` sink and three hooks — `beforeLoad` (MIB
Studio stops its live preview loop), `onFileChange` (host invalidates its
scheduler views) and `onInfo` (workflow facts, status bar).
`ReviewPanelHandle` exposes `openFile` / `openPath` / `closeFile` for menus.

The panel talks only to the **review bridge** (`src/review/reviewBridge.ts`
→ `src-tauri/src/review.rs` → `ReviewBridge` → [[../services/ReviewSession]]):
`open`/`close`, `info` (counts, ROI, series, accounting summary, the
recorded pixel-to-micron factor with a "(fallback)" marker — TD-17),
`rows` (full columns; the table shows index, object, track, area px²/µm²,
deformability, ring ratio, E), `frame(dataset, index, overlay, roi)` with the
overlay composed in the backend (Mono8 or RGB8 packets,
`packetToImageData`). Export Metrics, Export All, Batch Metrics, Batch Export All and
Regenerate masks start backend jobs (`ReviewJobs`) with native pickers; the
host's event drain logs their outcome. Progress/cancel dialogs and the
series-range prompt arrive with PR 4.

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
- **Launch file** — `review_launch_path` returns the first existing
  `.h5`/`.hdf5` argument (`yofo-review run.h5`, the Windows/Linux file
  association); `?open=<path>` does the same for dev and the screenshot
  harness. macOS Finder opens (`RunEvent::Opened`) land with PR 5.

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

## `ReviewApp` (the product shell)

Initializes the review bridge on boot (`init("")` → Tauri `app_data_dir`),
drains `poll_review_events` at 5 Hz for job outcomes, and renders the menu row (File ▸ Open…, View ▸ Fit, Help ▸ About
with the stamped version from `@tauri-apps/api/app`), the panel, a status
bar and the log drawer. No camera, experiment or hardware state exists.

## CI

`review-ci.yml` (Linux, headless): version-stamp check, backend archives,
contract drift gate, both pages built (`dist/review.html` must exist),
vitest, `cargo build --features review-only` under `TAURI_CONFIG`, a
`strings` check that the binary carries `bio.yofo.review`, an `nm` check
that it links no `backend::AppBackend` (and does link `ReviewSession`),
`cargo test --features review-only` in `crates/mib-bridge`, and the Xvfb
boot smoke. macOS and Windows bundle jobs arrive with the plan's PR 5/PR 6.

## Gotchas

- The overlay replaces the whole `app.windows` array (JSON merge patch), so
  the review window must restate `label: "main"` — `capabilities/default.json`
  targets that label for both products.
- The cargo binary is still named `mib-studio-desktop` in `target/`;
  `mainBinaryName` applies when the Tauri CLI bundles.
- Keep product identity in the config overlay and the feature flag only;
  no `#ifdef`-style branching in React sources (ADR 0008).
