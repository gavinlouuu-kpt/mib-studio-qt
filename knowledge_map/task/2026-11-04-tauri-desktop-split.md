# 2026-11-04 — Split the desktop classic view by area (#869 wave 0)

**Why:** seven parity streams (#862–#868) were about to edit `desktop/src/App.tsx` (one component,
2685 lines) in parallel. The split gives each stream its own files under `desktop/src/areas/<area>/`
with no change in behaviour. Branch `refactor/tauri-desktop-split`.

**How the split was kept behaviour-neutral**
- The moved code is byte-identical to the original lines: a generator spliced each original line
  range into the new files (only imports, the stage-tab conditions and closing tags were rewritten),
  and a coverage check confirmed every original line landed exactly once.
- Effect order is preserved: `studio/useStudioController.ts` calls the area hooks in the order their
  effects had in `App`. State-only hooks (`use<Area>State()`, no effects) come first, so cross-area
  setters (the startup recovery sets review and experiment state) need no cycles.
- `App.css` became an `@import` manifest in the old order; `vite build` produced the same CSS file
  (same content hash) before and after.
- The only test change: `components/ViewerGate.test.tsx` reads `App.tsx` + `areas/**/*.tsx` instead
  of `App.tsx` alone (its needles and assertions are unchanged).

**Where things are:** `docs/howto/desktop-area-ownership.md` (owner map, seams, rules, and a line
map from `App.tsx:<line>` at `5f906ac2` to the new files for the parity matrix's `tauri_ref`).
Plan: `docs/exec-plans/active/2026-11-05-tauri-parity-program.md`. See [[architecture/Desktop-Shell]].
