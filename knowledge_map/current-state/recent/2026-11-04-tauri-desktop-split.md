## 2026-11-04 — Desktop classic view split into area modules (#869 wave 0)

`desktop/src/App.tsx` (2685 lines) and `App.css` (1266 lines) are split by area so the seven Tauri
parity streams (#862–#868) edit separate files. No behaviour change: `App.tsx` only provides
`StudioContext` and lays out the views under `desktop/src/areas/<area>/`; each area's state is in
`use<Area>State()` and its callbacks and effects in named hooks, composed by
`desktop/src/studio/useStudioController.ts` in the original effect order. `App.css` is an `@import`
manifest of per-area style files in the old order (the built CSS is byte-identical). Owner map and
rules: `docs/howto/desktop-area-ownership.md`; program plan:
`docs/exec-plans/active/2026-11-05-tauri-parity-program.md`. See [[architecture/Desktop-Shell]] and
[[task/2026-11-04-tauri-desktop-split]].
