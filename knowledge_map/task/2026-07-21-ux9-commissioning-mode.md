Title: Operator vs Service/Commissioning mode (UX-9, issue #313 / epic #304)

Context:
- Manual sort-trigger and periodic-trigger tests actuate hardware but were
  mixed into the routine Monitoring toolbar. UX-9 separates a routine
  `operator` presentation from an explicit `service` (commissioning) mode:
  commissioning controls are hidden in operator mode, and actuation requires
  the mode + an explicit arm + a safe experiment state. Based on dev/react-tauri
  (UX-1/3/4/8 merged).

Implementation Notes:
- `desktop/src/commissioning.ts` — new pure module. `canActuate(input)` gates a
  one-shot actuation in strict order: must be in service mode → not during an
  active experiment → trigger attached → armed. `canStartPeriodic` shares that
  gate; `canStopPeriodic` is always allowed while a test is running (an obvious
  persistent Stop, even after the session reset to operator). `DEFAULT_MODE` is
  `operator`. No React/Tauri imports (unit-testable safety logic).
- `desktop/src/App.tsx`:
  - `operatingMode` state defaults to operator every session; a menubar
    "Mode: Operator / Service ⚠" toggle switches it, and entering service
    requires an explicit `window.confirm`. A persistent amber banner shows while
    in service mode with an "Exit to Operator" button. Mode changes are logged.
  - The Monitoring trigger controls (Sort Trigger, Set Pulse, Periodic Test,
    interval) render only in service mode, behind an **Arm** checkbox; buttons
    are gated by the `commissioning.ts` checks and show the block reason as the
    tooltip. Arming is one-shot (cleared after firing) and auto-clears when
    leaving service mode or when an experiment goes active.
  - Safety fallback: if a periodic test is running while in operator mode, a
    standalone "Stop Periodic Test" button stays visible.
- `desktop/src/App.css` — `.mode-toggle` / `.service-banner` / `.arm-toggle` /
  `.commissioning-hint`.

Verification:
- `desktop/src/commissioning.test.ts` — 9 vitest cases: default mode, each gate
  in `canActuate` (operator, active experiment, unattached, unarmed, allowed),
  periodic start parity, and stop-always-allowed / stop-noop.
- `npm run build` (tsc strict + vite) green; `npm test` green (64 total).
  Headless Chromium render confirms the toggle enters service mode (via the
  confirm) and the banner appears, with no page errors.
- `python3 scripts/check_docs.py` green.

Follow-ups:
- Boot-service toggles, raw camera scripts, and low-level processing-core
  actions could move behind the same Service mode as their UIs land.
- Recording commissioning actuations into experiment provenance (when they
  occur during a run) needs the provenance surface (UX-10 / #274).
