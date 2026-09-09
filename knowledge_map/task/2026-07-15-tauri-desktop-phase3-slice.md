# Qt → React/Tauri migration: Phase 3 — first Tauri vertical slice (mock camera)

Date: 2026-07-15
Epic: #246 · ADR: `docs/decisions/0003-rust-cxx-bridge.md` ·
Plan: `docs/exec-plans/active/2026-07-15-qt-decoupling-and-tauri-migration.md`

## Context

Phase 2 shipped the Rust ↔ C++ bridge (`crates/mib-bridge`). Phase 3 builds the
first React + Tauri v2 vertical slice on top of it: the **mock camera end to
end**. The anticipated hard block was webkit2gtk / needing a display — both
turned out to be surmountable (webkit deps install on `ubuntu-24.04`; the GUI
runs headless under Xvfb), so there was no intervention-requiring block.

## What shipped

New `desktop/` app (root `src/` is the C++ tree, so the Tauri app is nested):

- `desktop/src-tauri/` — Tauri v2 Rust app. `AppState` holds
  `Mutex<UniquePtr<BackendBridge>>` + a cached last-frame buffer; the
  `#[tauri::command]` layer wraps the bridge: `abi_version`, `is_initialized`,
  `init`, `configure_mock`, `start_capture`, `stop_capture`, `seek_latest`,
  `poll_events` (→ serde `EventDto[]`), `fetch_frame` (→ `FrameMeta`), and
  `frame_bytes` (→ `tauri::ipc::Response` — raw Mono8 bytes, **no base64**).
  Crate-type `rlib` (binary), not `cdylib`.
- `desktop/src/` — React + Vite + TS. `bridge.ts` is the typed IPC client +
  `mono8ToImageData`; `App.tsx` is the slice UI (init → mock dir → start/stop →
  live canvas + event log).
- `desktop/scripts/xvfb-smoke.sh` — headless GUI smoke launcher.
- `.github/workflows/desktop-ci.yml` — frontend build + Tauri build + `cargo
  test` + Xvfb smoke.

## Verification

- Frontend `tsc && vite build` — clean.
- `cargo test` (headless): `mock_camera_slice_round_trip` drives init → configure
  → start → pull-frame (asserts 512×96) → stop → shutdown on the bridge from the
  desktop crate; `event_kind_names_are_stable`. **2/2 green.**
- Xvfb smoke: the real binary launches its GTK/WebKit window and stays alive
  under `xvfb-run` with `WEBKIT_DISABLE_DMABUF_RENDERER=1` +
  `WEBKIT_DISABLE_COMPOSITING_MODE=1` + `LIBGL_ALWAYS_SOFTWARE=1`. **OK.**

## Integration findings (fed back into Phase 2)

- The bridge object had to be marked `Send` (`unsafe impl Send`, never `Sync`)
  so a Tauri `State<Mutex<UniquePtr<BackendBridge>>>` is `Send + Sync` — added to
  `crates/mib-bridge` with a `const _` compile-time guard.
- The non-PIC C++ archives can't link into Tauri's default `cdylib`/`staticlib`
  (mobile) crate-types (`recompile with -fPIC`). Desktop uses binary + `rlib`,
  which links as an executable. Documented in
  `knowledge_map/architecture/Rust-Bridge.md`.

## Result

Phase 3 slice-0 is done: a headless-verifiable React + Tauri app that drives the
Qt-free backend through the bridge and renders live mock-camera frames. Next is
Phase 4 — migrating the remaining operator workflows (hardware camera,
recording, review, experiment/processing UI, syringe pump, autofocus).
