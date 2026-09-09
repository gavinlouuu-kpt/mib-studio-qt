Title: Camera & Alignment image-quality gates (UX-4 slice, issue #308 / epic #304)

Context:
- UX-1 (#305) gave the Camera & Alignment stage a Ready→Complete confirmation,
  but "is the image good?" was a judgement call. This adds concrete quality
  gates derived from already-bridged signals so focus / background / ROI /
  calibration each show pass/warn/fail with an explanation — the detailed
  content of the Align stage, mirroring the UX-3 preflight checklist. Stacked
  on UX-3 (#317, now merged).
- This is the quality-gates slice of UX-4, not the whole issue. ROI *editing*
  stays with UI-2 (#267); save-to-profile needs profile management (UX-2 #306);
  illumination stability/saturation and channel-wall ROI insets (#295) need
  signals that are not bridged yet. All are noted as follow-ups.

Implementation Notes:
- `desktop/src/quality.ts` — new pure module. `deriveQualityGates(input)`
  returns Focus / Background / ROI / Calibration gates, each a
  `pass`/`warn`/`fail`/`unknown` status with a short value and explanation, plus
  pass/warn/fail/unknown counts. No React/Tauri imports (unit-testable).
  - Focus: autofocus ring-ratio metric + freshness (schema v11). Unknown until
    the camera runs / the controller is connected; warn if never reported or
    stale (age > threshold); pass with the live value otherwise.
  - Background: config `background_set` → pass, else warn.
  - ROI: config ROI vs current frame size — warn if unset, fail if larger than
    the frame, pass with `WxH` otherwise.
  - Calibration: px→µm > 0 → pass with value, else warn.
- `desktop/src/App.tsx` — builds the input from existing state (autofocus
  status, config background/ROI, last frame size, px→µm) and renders the gates
  strip directly under the live image in the Camera & Alignment (Overview) tab
  (status = text + dot, never colour alone). Focus staleness uses a
  conservative default until the autofocus config's `ring_ratio_stale_ms` is
  polled into the shell.
- `desktop/src/App.css` — `.quality-panel` / `.quality-gate` / `.gate-dot`.

Verification:
- `desktop/src/quality.test.ts` — 12 vitest cases: focus unknown/stale/live,
  background, ROI unset/oversize/valid, calibration, and count accounting.
- `npm run build` (tsc strict + vite) green; `npm test` green (44 total with
  UX-1 + UX-3). Headless Chromium render shows the 4-gate strip in the
  Camera & Alignment tab with no page errors.
- `python3 scripts/check_docs.py` green.

Follow-ups (remainder of UX-4):
- Illumination stability/saturation gate + background freshness (need bridged
  signals); channel-wall ROI-inset warning from #295; Auto Focus/Optimize
  action; save-adjustments-to-local-profile (UX-2 #306); effective-setting
  source labels; ROI editing accuracy under zoom/pan/DPI with UI-2 (#267).
