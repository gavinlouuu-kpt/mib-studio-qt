## 2026-11-05 — Monitoring click to view, target-group gate on the scatter, E-modulus table, camera telemetry (#866 PR E1, MON-043/044/046..048/064..068/077/095/098)

The Tauri Monitoring sub-tab gets the Qt tab's last interactive parts, on the bridge ABI 46 the backend half
of this PR added (no backend change here). See [[architecture/Desktop-Shell]] ("Monitoring click to view, gate
overlay and E table") and [[frontend/ExperimentMonitoringTab]] (the Qt behaviour).

- **Click to view.** A click on a scatter point selects the cell: a pane docked at the top of the
  histogram's card shows it (hidden until the first click), with Qt's title ("... · no longer on the scatter"
  once the row left the plotted window), Previous / Next through the plotted points in buffer order
  (wrapping; from a vanished selection they start at an end), Open in window… (a non-modal floating viewer
  inside the app that follows the selection) and ✕ (clears the selection; so does Clear Buffer). The pane
  pins the cell in the backend (`monitoring_cell_pin`, a control command), pulls its crop and mask by identity
  and releases the pin when the selection moves, on close and on unmount, in strict order (a selection that
  moves while a pin is in flight unpins the stale pin first; max 16 pins, never leaked); a viewer client does
  not pin. PinnedWithoutImage shows the measurements and the reason, CaptureMismatch / RunMismatch / NotFound /
  LimitReached a plain message. The picture is the backend's object-centred crop, not Qt's whole ROI frame
  (#880), with the valid strip's Show Overlay drawing the mask.
- **Gate overlay.** `TargetGatePreview` ported to TypeScript (`areas/monitoring/gate/targetGate.ts`, with the
  C++ test cases): the draft target-group gate is drawn on the scatter (tint, dashed box, dash-dot iso-E
  contours with "E ≥ x kPa" labels, "Target group" / "(unapplied)" pill, the red "No E-modulus table loaded"
  pill) and dragged into the tune draft (edges and E contours, 6 px, clamp / round / min ≤ max, Apply still
  required). Hover shows a resize cursor and "... — drag to change". The live count "Target group preview: N of
  M plotted valid cells (P%)" sits under the scatter. `ScatterOverlay.hover` may now return
  `{cursor, tooltip}` and the overlay gets `onLeave` (additive).
- **E-modulus table.** Fetched when the sub-tab becomes visible (with the identity held, so the grid comes back
  only when it differs; never on the poll), `EModulusLut::lookup` ported (`emodulusLut.ts`): the empty-plot
  tooltip reads "· E 1.52 kPa", the tune panel's Young's-modulus range says whether a table is loaded.
- **Statistics.** `fetch_processing_stats` is decoded once per poll for the algorithm time AND the camera
  telemetry; the sidebar's Camera FPS / Data Rate rows show the backend's samples in Qt's words and fall back
  to the rate measured here (with a tooltip) only when the backend sends none. `statsReadouts` uses the bridge
  decoder's `MetricSample` and `formatMetricSample`.
- **Shared files touched, additively:** `bridge.ts` (`fetchProcessingStats` attaches the telemetry),
  `eventAdapter.ts` (two optional members of `ProcessingStats`), `App.css` (one `@import`),
  `studio/useStudioController.ts` (two hook calls), `areas/shell/Sidebar.tsx`.
- **Tests.** `gate/targetGate.test.ts`, `gate/gateOverlay.test.ts`, `gate/gateHandles.test.ts`,
  `gate/useGateOverlay.dom.test.tsx`, `clickView/selection.test.ts`, `clickView/useCellView.dom.test.tsx`,
  `LiveMonitoringCharts.dom.test.tsx`, `useMonitoringView.dom.test.tsx`, `emodulusLut.test.ts`,
  `statsTelemetry.test.ts`, and additions to the scatter, strips, tune panel and sidebar tests.
