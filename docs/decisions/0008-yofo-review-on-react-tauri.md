# 0008. YOFO Review ships the Review tab as a React + Tauri product

Date: 2026-10-01
Status: accepted

## Context

Operators want to review recorded HDF5 files away from the acquisition
rig, on macOS and Windows laptops with no camera, framegrabber or serial
hardware. Today the review workflow exists only as the `HdfReviewTab` of
MIB Studio Qt (Windows-only installer, no macOS build at all) and, partially,
as the Review panel of the React + Tauri shell under `desktop/`.

Two shells were considered for the standalone product:

- **Qt, reusing `HdfReviewTab`.** Complete and tested today; the tab's only
  link to the application is one `AppBackend&` used for
  `processing()`. It would need a new macOS build chain for Qt, a second
  Qt product to carry through the Tauri cutover, and a Windows-only
  updater.
- **React + Tauri, extending the `desktop/` shell.** [ADR 0001](0001-react-tauri-migration.md)
  already names it the one supported shell once parity is reached. The
  Qt-free backend holds every piece of review science (`Hdf5Service`,
  `HdfExportService`, `ProcessingService::processBatch`, `KdeCoreRecord`,
  `MonitoringDensityService`), the bridge and contract exist, and Tauri
  bundles and updates on both platforms. The gap is presentation: charts,
  thumbnails, overlays, exports and jobs are not in the React panel yet,
  and the facade serves review from the experiment writer's HDF5 handle.

The full survey, decision log and PR breakdown are in
[`../exec-plans/active/2026-10-01-standalone-review-app.md`](../exec-plans/active/2026-10-01-standalone-review-app.md).

## Decision

1. **YOFO Review is a React + Tauri product built from the `desktop/` tree.**
   The same crate and React sources build two products by configuration:
   cargo feature `review-only` (registers only review, platform and dialog
   commands), `desktop/src-tauri/tauri.review.conf.json` (product name,
   identifier `bio.yofo.review`, binary `yofo-review`, window, bundle
   targets, file associations) applied with `--config`, and a second Vite
   page `review.html` → `src/review/main.tsx`. A separate app directory
   was rejected because it would fork the bridge client, frame-packet and
   scheduler modules.
2. **One review module serves both products.** `desktop/src/review/`
   (`ReviewPanel` and what follows it) is mounted by YOFO Review as the
   whole window and by MIB Studio in its Review tab. Delivering YOFO
   Review therefore delivers UI-4 (#269) for MIB Studio.
3. **One review implementation in the backend.** A Qt-free `ReviewSession`
   in a new `mib_review_core` library (built on `mib_processing`, never on
   `mib_backend`) owns the open file, every read and every job; the
   `BackendFacade` delegates its review surface to it. The review binary
   links no camera, serial, discovery, SQLite, curl or Sentry code
   (bridge feature `review-only`). Shells never implement science; overlays
   are composed in the backend and chart pixels are rendered by the shell.
4. **Shared version, separate identity.** The version is the repository's
   (`cmake/MIBVersion.cmake`), stamped into the Tauri configs by
   `scripts/release/stamp-tauri-version.py` and checked in CI, so one
   `vX.Y.Z` tag releases MIB Studio Qt, MIB Studio (Tauri) and YOFO Review
   together. Updates use `tauri-plugin-updater` against
   `updates.yofo.bio/review-<channel>/latest.json`.
5. **The Qt `HdfReviewTab` is frozen** apart from TD-17 (recorded
   pixel-to-micron factor) and keeps shipping in MIB Studio Qt until the
   Tauri cutover of ADR 0001.

## Consequences

- **Easier:** macOS and Windows from one build system (`tauri build`),
  in-place updates on both, a review product that is also the Tauri
  shell's Review tab, review bugs fixed once in `ReviewSession` for Qt,
  Tauri and YOFO Review alike.
- **Harder / must respect:** every review capability must be exposed
  through `ReviewSession` and the additive bridge contract (ADR 0004)
  before a shell can use it; the review binary must stay free of
  `mib_backend` (CI checks the link); product identity lives only in the
  config overlay, never in `#ifdef`s or duplicated sources; the Linux
  lane cannot bundle, so macOS and Windows jobs own packaging and must
  stay green.
- **Not signed, not notarised** (decision 2026-10-01): the app is not
  distributed through a store, so artefacts ship unsigned and the manual
  documents the one-time Gatekeeper / SmartScreen step. The only key is
  the self-issued minisign pair that `tauri-plugin-updater` verifies
  downloads against.
