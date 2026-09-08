Title: Persistent active-context bar (UX-8, issue #312 / epic #304)

Context:
- The storyboards put a bar across the bottom of every stage answering, at a
  glance: which profile, which camera, is calibration valid, who is operating,
  where is data written, and is the system ready / warning / blocked. This adds
  it, reusing the guided-workflow readiness (UX-1), preflight (UX-3), and
  quality (UX-4) reports the shell already computes. Based on dev/react-tauri
  (UX-1/3/4 merged).

Implementation Notes:
- `desktop/src/contextBar.ts` — new pure module. `deriveContextBar(facts)`
  returns seven segments (Profile, Camera, Calibration, Status, Operator,
  Storage, Warnings), each with a value, a `ok`/`warn`/`blocked`/`pending`/
  `neutral` status, a detail string, and an optional `tab` to navigate to.
  Exported `baseName()` gives a compact storage label. No React/Tauri imports
  (unit-testable).
  - Status mirrors the guided workflow: Running / Failed / Complete / the
    current stage's title+status.
  - Warnings = unresolved attention items (preflight failed+warning + quality
    warn+fail).
  - Camera/Calibration come from the bridged selection + px→µm.
- `desktop/src/App.tsx` — builds the facts from existing state and renders the
  bar as a persistent row above the status bar, present on all four stages.
  Segments with a `tab` are clickable and navigate without losing workflow
  state; status is text + dot (never colour alone) with the detail as tooltip
  and an accessible `aria-label` exposing the full value.
- `desktop/src/App.css` — `.context-bar` / `.context-seg` / `.seg-dot`.

Honest gaps (follow-ups):
- **Profile** (Experiment Profile name/revision/applied) is `pending` until
  profile management is bridged (UX-2 #306).
- **Operator** identity is not captured yet → `pending`.
- **Storage** shows the output file name during a run; free-space and a
  writable-path check are `pending` a backend storage-status contract (same gap
  noted in UX-3).

Verification:
- `desktop/src/contextBar.test.ts` — 11 vitest cases: segment order, pending
  vs available profile/operator, camera blocked/idle/streaming, calibration,
  warnings, system Running/Failed/Complete/stage-mirror, storage pending→named,
  and `baseName` POSIX/Windows/trailing-slash.
- `npm run build` (tsc strict + vite) green; `npm test` green (55 total).
  Headless Chromium render shows the 7-segment bar on every stage, no page
  errors.
- `python3 scripts/check_docs.py` green.
