Title: React/Tauri desktop shell — Qt operator UI parity (UI-1, issue #266)

Context:
- Replaced the developer-oriented Phase 3/4 form in `desktop/src/App.tsx` with
  the operator workflow and information hierarchy of the Qt app on `main`
  (parity source: `docs/manual/images/`). Parity task under epic #246; first
  UI issue of the React/Tauri breakdown (#266–#279).
- New regions: File / Settings / Help menu row, persistent collapsible
  telemetry sidebar, Connect / Overview / Experiment / Review tabs with
  Start/Stop Camera in the tab header, nested Preview / Monitoring and
  App config / Camera script tabs, Review frame + metrics-table split, and a
  metrics status bar with a toggleable log drawer.

Implementation Notes:
- `desktop/src/App.tsx` — full shell rewrite; `desktop/src/App.css` — new
  layout stylesheet (grid: menubar / sidebar+main / statusbar).
- Everything implemented in bridge schema v3 stays wired: backend auto-init on
  boot (empty data dir → Tauri `app_data_dir`), Configure Mock… modal
  (`configure_mock`), Start/Stop Camera (`start/stop_capture`), live canvas
  via `fetch_frame` + binary `frame_bytes`, Record raw frames
  (`start/stop_recording`), realtime processing toggle + px→µm
  (`apply_processing`, `fetch_processing_stats`), Review load + scrub
  (`load_recording`, `seek_index`, `fetch_frame_by_index`).
- Un-bridged controls render visible but disabled with a tooltip naming the
  blocking backend issue (BE-2 #272 discovery/script, BE-3 #273 ROI/config/
  profiles, BE-4 #274 experiment, BE-5 #275 monitoring/trigger, BE-6 #276
  review metrics/export, BE-8 #278 autofocus, BE-9 #279 platform/shell).
  No backend or hardware state is simulated: telemetry rows without a data
  source show "—", and Display FPS / data rate are measured UI-side from
  frames actually drawn (labelled as such; authoritative backend metrics are
  UI-2/BE-2 scope).
- Sidebar collapse state persists via `localStorage` (`mib.sidebar.collapsed`).
- Review keeps the Qt tab set visible (Valid/Invalid Frames, Charts disabled →
  BE-6) and adds an active "Raw Frames" tab for the scrub capability that the
  bridge actually has today.

Verification:
- `npm run build` (tsc strict + vite) green.
- `python3 scripts/check_docs.py` green.
- Xvfb smoke (`desktop/scripts/xvfb-smoke.sh`) — shell boots headless.

Follow-ups:
- UI-2 (#267) Connect/Overview completion, UI-3 (#268) Experiment/Monitoring,
  UI-4 (#269) Review, UI-5 (#270) visual-regression/accessibility gates; the
  backend surfaces land via BE-1…BE-9 (#271–#279).
