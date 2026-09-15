Title: Guided four-stage operator workflow (UX-1, issue #305 / epic #304)

Context:
- The Qt-parity shell (#266) presents Connect / Overview / Experiment / Review
  as *locations*: visiting a tab says nothing about whether its work is done,
  and auto-navigation can imply hardware is ready when it is not. UX-1 layers
  an authoritative *stage state* on top of those tabs so the operator can see
  where they are, what remains, which stages are ready or blocked, why, and the
  next safe action.
- First issue of the UX redesign epic #304 (a deliberate redesign tracked
  separately from the #246 React/Tauri parity work). Detailed content of each
  stage is out of scope here and tracked by UX-2…UX-11 (#306–#315).

Implementation Notes:
- `desktop/src/workflow.ts` — new pure module. `deriveWorkflow(facts)` maps the
  four stages to a status (`not-started` / `needs-attention` / `ready` /
  `running` / `complete`), each with a human summary, the specific blocking
  checks, and the single recommended next action. No React/Tauri imports, so it
  is unit-testable in plain Node.
- Facts are authoritative backend snapshots the shell already polls — the
  module invents no state: `CameraSelection.configured`/running, capture
  `running`, `ProcessingCoreStatus` (valid + pin_satisfied), `ExperimentStatus`,
  `ReviewMetadata` (file_open + valid).
- **Preflight completion requires explicit operator confirmation**, never mere
  detection: critical checks passing yields `ready`, and only a confirmation
  matching the current device+core *signature* yields `complete`. Changing the
  device or pinned core invalidates the confirmation automatically (the stored
  signature no longer matches). Same pattern for Camera & Alignment. Both
  confirmations live in React state (not persisted), so every session
  re-confirms readiness.
- `deriveWorkflow` takes no notion of the active tab, so visiting a stage can
  never complete it (locked down by a unit test).
- `desktop/src/App.tsx` wiring: each tab renders its stage title + status
  (text **and** a colour dot — never colour alone) with the blocking checks as
  the tooltip and an accessible `aria-label`; a "Next" banner shows the current
  stage summary and a single action button (navigate, or confirm
  preflight/alignment — the confirm is disabled while a run is active); startup
  lands on the earliest incomplete stage unless an active/failed experiment
  requires the Experiment stage.
- `desktop/src/App.css` — `.stage-tab` / `.stage-dot` (status colours incl. new
  `--warn`) and `.workflow-next` banner.

Verification:
- `desktop/src/workflow.test.ts` — 18 vitest cases: detection-alone-≠-complete,
  confirm→complete, signature-change invalidation, core-pin block, stage
  gating, running/complete/review transitions, and the "pure function / visiting
  never completes" invariant. New `npm test` script + a Desktop CI step.
- `npm run build` (tsc strict + vite) green; `npm test` green.
- Headless Chromium render of the built app: the four stage tabs mount with
  correct "Not started" state and no page errors.
- `python3 scripts/check_docs.py` green.

Follow-ups:
- UX-2 profile-first setup (#306), UX-3 preflight checklist content (#307),
  UX-4 alignment workspace (#308), UX-6 readiness gate (#310) refine what feeds
  each stage's facts. When the backend exposes a device/status snapshot for
  stage/pumps/trigger/storage, extend `WorkflowFacts` accordingly.
- Xvfb GUI smoke proves boot only (debug binaries load `devUrl`); a full
  driven-UI stage walkthrough belongs with UX-11 (#315) E2E coverage.
