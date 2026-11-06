## 2026-11-05 — Tauri Monitoring charts rebuilt, Monitoring Settings dialog added (#866, MON-036..043, 049..051, 082, 087..091)

The Monitoring sub-tab's charts in the React/Tauri app now behave like Qt's `ExperimentMonitoringTab`.
They are canvas charts under `desktop/src/areas/monitoring/charts/` over Qt's plotted window (the
poll asks for 1015 rows, the backend ring's 1000 newest valid rows plus 15 for the latest-rows table,
instead of a hard-coded 200; a poll with nothing new keeps the old snapshot so nothing re-renders). Scatter: fixed default axes
(X 0-1000 µm², Y 0-1) that hold between polls, wheel zoom around the pointer (Ctrl X, Shift Y, over an
axis that axis), drag/middle-drag pan, double-click on empty space resets, isoelastic curves always drawn
with a "x.xx kPa" legend on the right, pointer tooltips ("Area · deformability", "Frame N · click to
view" with a hand cursor). Ring histogram: Qt binning ((X max - X min) / bin width), fixed Y max, zoom and
pan, X range = the runtime config's `ring_ratio_min/max` when the sub-tab is shown and when the config
changes. The dead invalid-object chart code is gone.

Settings > Monitoring Settings... now opens a modal dialog (OK / Cancel / Apply) with the scatter X/Y
range, histogram X range, Y max and bin width (Qt ranges, decimals and defaults); before it only
switched to the Monitoring sub-tab. The values are kept in memory like Qt's. Seams left for the
click-to-view pane (`onSelectPoint`, `selectedKey`), the target-group gate (`overlay`) and the E readout
(`emodulusAt`, waits for an E table in the bridge). See [[architecture/Desktop-Shell]] and
[[frontend/ExperimentMonitoringTab]] (the Qt behaviour).
