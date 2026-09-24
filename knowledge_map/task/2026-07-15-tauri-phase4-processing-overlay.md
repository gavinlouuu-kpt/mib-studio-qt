# Qt → React/Tauri migration: Phase 4 slice 2 — processing settings + stats overlay

Date: 2026-07-15
Epic: #246 · ADR: `docs/decisions/0003-rust-cxx-bridge.md` ·
Plan: `docs/exec-plans/active/2026-07-15-qt-decoupling-and-tauri-migration.md`

## Context

Phase 4 slice 2 surfaces the realtime processing pipeline in the `desktop/`
Tauri app: enable/disable realtime processing, set the pixel→micron scale, and
show a live fps overlay. The facade previously emitted a `ProcessingResultEvent`
only as a one-shot snapshot on settings-apply — not a continuous stream — so a
**pull** (symmetric with the frame pull) is the clean fit for live metrics.

## What shipped

Backend seam (`BackendFacade`):
- New `BackendProcessingStats { algoFps1s, validFps1s, invalidFps1s,
  pixelToMicronFactor }` + `bool fetchProcessingStats(out) const` reading the
  `ProcessingService` atomic getters. Additive; existing facade tests unaffected.

`crates/mib-bridge` (schema **v2 → v3**, additive):
- Command `apply_processing(realtime_enabled, pixel_to_micron)` (dispatches
  `ProcessingSettingsCommand`) and pull `fetch_processing_stats() ->
  BridgeProcessingStats`. `bridge_abi_version()` → `3`.
- Contract test `processing_settings_and_stats`.
- `build.rs` now `rerun-if-changed` on the backend archives so a facade edit
  reliably relinks (no stale-symbol builds).

`desktop/`:
- Tauri commands `apply_processing`, `fetch_processing_stats`.
- UI: a Processing panel (realtime checkbox + pixel→micron input + Apply + a
  live `algo/valid/invalid fps` overlay polled each tick).
- Desktop test `processing_settings_round_trip`.

## Verification

- `mib-bridge` `cargo test` — 4/4 (abi v3, live lifecycle, record/review,
  processing).
- `desktop` `cargo test` — 4/4 (+ `processing_settings_round_trip`).
- Frontend `tsc && vite build` — clean. Xvfb GUI smoke — green.

## Result

The desktop app now drives capture → record → review → **realtime processing
settings + live metrics**. Remaining Phase 4 slices: camera selection,
experiment run + monitoring, syringe pump, autofocus/nanopositioner.
