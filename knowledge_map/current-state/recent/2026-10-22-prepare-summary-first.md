## 2026-10-22 — Summary-first preflight: the model and the disclosure row (O1b, WIP)

The shell-independent parts of Gavin's approved O1b "summary-first" preflight, on the workspace
branch. Nothing is mounted yet: where the summary lands (the classic Hardware Preflight stage) and
where the hardware forms go are being designed in Penpot, and no view is built before Gavin
approves those boards.

- **`desktop/src/workspace/prepareSummaryModel.ts`** (pure): what counts as an issue under Gavin's
  decisions of 2026-10-22. The gates a running camera or Align resolves (camera.session,
  camera.deliveryMode, camera.geometry, telemetry.transportLoss, processing.roi,
  processing.background) are not counted: `nextAlignText` puts them on a neutral "Next · Align"
  line ("start the camera, set ROI and background · Start waits for these 6 gates"); the backend
  still refuses Start until they pass. The mock camera chosen on purpose (camera.source Warn,
  "simulated camera source selected") is not counted and reads "Hardware ready · simulated camera";
  a fallback (camera.source Fail) counts. storage.output is never counted. Checklist checks count
  as `prepareVerdict` counts them; gates not read count as one unknown. Also the headline
  (`prepareHeadline`), the one-line note beside Confirm (`startRefusalLine`), the "detected and
  selected" line, row chips, the All checks summary, the row of each gate (`gateRow`) and the
  Calibration row's auto-open (`calibrationRowState`). `prepareVerdict`'s words (header chip, Plan)
  are unchanged.
- **`desktop/src/workspace/DisclosureRow.tsx`** + `disclosure.css`: a native `<details>` styled with
  `--mib-*` tokens (kit gap #715) with a controlled summary click; `data-open` tells an operator's
  open (`user`) from an automatic one (`issue`, `auto`). A row with an issue cannot be collapsed;
  `isShown` checks visibility against closed `<details>` (textContent counts hidden content).

Tests: `prepareSummaryModel.test.ts`, `DisclosureRow.test.tsx`. See [[architecture/Desktop-Shell]].
