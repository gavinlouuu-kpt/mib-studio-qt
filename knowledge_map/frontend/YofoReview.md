# YOFO Review (React + Tauri)

> The standalone review product: opens the HDF5 files MIB Studio records on
> macOS and Windows with no camera, hardware or experiment code. Built from
> the same `desktop/` tree as the React + Tauri MIB Studio shell.

**Source:** `desktop/review.html`, `desktop/src/review/` (`main.tsx`,
`ReviewApp.tsx`, `ReviewPanel.tsx`, `review.css`),
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

`src/review/ReviewPanel.tsx` owns the review state (file path, metadata,
metrics page, selected image, sub-tab) and the `"review"` slot of the
host's `FramePullScheduler`. The host supplies: `ready` (backend
initialized), the scheduler, the `PlaybackPosition` range it drains from
`poll_events`, `fitWindow`, a `log` sink, `applyEvents`, and three hooks —
`beforeLoad` (MIB Studio stops its live preview loop), `onFileChange`
(host invalidates its scheduler views) and `onMetadata` (workflow facts,
status bar). `ReviewPanelHandle.openFile()` is the File ▸ Open… action for
menus.

The panel talks only to the **review bridge** (`src/review/reviewBridge.ts`
→ `src-tauri/src/review.rs` → `ReviewBridge` → [[../services/ReviewSession]]):
`open`/`close`, `info` (counts, ROI, series, accounting summary, the
recorded pixel-to-micron factor with a "(fallback)" marker — TD-17),
`rows` (full columns; the table shows index, object, track, area px²/µm²,
deformability, ring ratio, E), `frame(dataset, index, overlay, roi)` with the
overlay composed in the backend (Mono8 or RGB8 packets,
`packetToImageData`). Raw Frames scrub the file's `/recorded_frames`
dataset, never the live FrameStore. Export Metrics, Export All, Batch Metrics, Batch Export All and
Regenerate masks start backend jobs (`ReviewJobs`) with native pickers; the
host's event drain logs their outcome. Progress/cancel dialogs and the
series-range prompt arrive with PR 4; thumbnails, charts and the viewer
with PR 2–3.

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
