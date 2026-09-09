Title: Profile-aware hardware preflight checklist (UX-3, issue #307 / epic #304)

Context:
- UX-1 (#305) gave the Preflight stage a status but only knew "camera
  configured + core pinned". UX-3 turns it into an explicit per-subsystem
  checklist so the operator can distinguish "detected" from "ready for this
  experiment", see expected-vs-detected identity, and get a recovery hint for
  anything not ready. Stacked on the UX-1 branch (PR #316).

Implementation Notes:
- `desktop/src/preflight.ts` — new pure module. `derivePreflight(input, reqs)`
  produces a checklist of camera / processing-core / capture-stream / autofocus
  / sample pump / sheath pump / trigger / storage checks, each with a status
  (`passed` / `warning` / `failed` / `not-required`), the requirement
  (`required` / `optional` / `not-applicable`), expected + detected identity, a
  human cause/next-step, and recovery actions. `criticalPassed` is true only
  when every `required` check passed. No React/Tauri imports (unit-testable).
- Which optional devices are required/optional/not-applicable is meant to come
  from the selected Experiment Profile. Profile management is not bridged yet
  (BE-3 #273 follow-up / UX-2 #306), so `DEFAULT_REQUIREMENTS` (camera + core
  required; autofocus/pumps/trigger/storage optional) applies until a profile
  can declare them — the `PreflightRequirements` shape is ready for that.
- `desktop/src/App.tsx` — a `refreshPreflight` poll fetches camera selection,
  core status, autofocus, both pumps, and trigger every 1.5 s while the
  Preflight (Connect) stage is shown, *off the capture loop* so preflight works
  before the camera starts. The checklist panel renders in the Connect tab with
  status (text + dot, never colour alone), `required` badges,
  expected/detected, per-check Refresh/Retry recovery, and a gate footer.
- `desktop/src/App.css` — `.preflight-panel` / `.preflight-check` /
  `.check-dot` / `.req-badge` styles (reusing `--ok/--warn/--err`).

Honest gaps (follow-ups):
- Storage readiness (path writability + free space) has no authoritative bridge
  contract yet, so that check is informational (`not-required`) with a note.
- Profile-declared required devices + expected camera identity arrive with UX-2
  (#306); the module already accepts them (`requirements`, `cameraExpected`).
- Recovery is limited to Refresh/Retry (re-poll); Reconnect/Open-settings need
  the per-device panels (BE-7/BE-8 UI), a later slice.

Verification:
- `desktop/src/preflight.test.ts` — 14 vitest cases: backend-down, no-camera,
  healthy pass, expected-identity mismatch warning, core-pin block, optional vs
  profile-required device, not-applicable, trigger, storage informational +
  unwritable, capture pending→streaming, and status-count accounting.
- `npm run build` (tsc strict + vite) green; `npm test` green (32 total with
  UX-1). Headless Chromium render confirms the 8-row checklist mounts with no
  page errors.
- `python3 scripts/check_docs.py` green.
