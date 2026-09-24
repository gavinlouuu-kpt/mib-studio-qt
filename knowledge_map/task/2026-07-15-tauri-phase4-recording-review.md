# Qt → React/Tauri migration: Phase 4 slice 1 — recording + review

Date: 2026-07-15
Epic: #246 · ADR: `docs/decisions/0003-rust-cxx-bridge.md` ·
Plan: `docs/exec-plans/active/2026-07-15-qt-decoupling-and-tauri-migration.md`

## Context

Phase 3 landed live mock-camera capture in the `desktop/` Tauri app. Phase 4
migrates the remaining operator workflows one slice per PR. Slice 1 is the
**record → review** loop: record the live stream to HDF5, then load it back and
scrub by frame index — the backend/facade already support this
(`RecordingLoadCommand`, `PlaybackSeekCommand(AbsoluteIndex)`,
`fetchFrameByIndex`), it just wasn't exposed through the bridge yet.

## What shipped

`crates/mib-bridge` (schema **v1 → v2**, additive):
- New commands `load_recording(path)`, `playback_seek_index(index)`, and frame
  pull `fetch_frame_by_index(index)`. `bridge_abi_version()` → `2`.
- Contract test `record_then_load_and_review`: record → `load_recording` →
  `playback_seek_index(0)` → `fetch_frame_by_index(0)`.

`desktop/`:
- Tauri commands `start_recording`, `stop_recording`, `load_recording`,
  `seek_index`, `fetch_frame_by_index` (frame pulls cache bytes for
  `frame_bytes`, same binary/no-base64 path).
- UI: a **Recording** panel (record the live mock stream to an HDF5 path) and a
  **Review** panel (load a recording + a scrubber bounded by `PlaybackPosition`
  events; each scrub seeks + pulls-by-index + redraws the canvas).
- Desktop test `record_and_review_round_trip`.

## Verification

- `mib-bridge` `cargo test` — 3/3 (abi v2, live lifecycle, record/review).
- `desktop` `cargo test` — 3/3 (kind names, mock slice, record/review).
- Frontend `tsc && vite build` — clean.
- Xvfb GUI smoke — window + webview stay alive.

## Result

The desktop app is now capture → record → review. `desktop-ci.yml` (build +
test + Xvfb smoke) and `bridge-ci.yml` cover it headless. Next Phase 4 slices:
hardware/MindVision camera selection, live processing settings + results
overlay, experiment run + monitoring, syringe pump, autofocus/nanopositioner.
