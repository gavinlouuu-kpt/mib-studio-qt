## 2026-11-05 — Tauri Monitoring scatter: density (KDE) colouring, core contour, reference contour (#866 PR E2, MON-058..063, 083..086)

The Monitoring scatter of the React/Tauri app now has Qt's "Density (KDE)" mode on the bridge ABI 46
density surface (see [[current-state/recent/2026-11-05-w1-monitoring-density-bridge]]). Behaviour
reference: [[frontend/ExperimentMonitoringTab]] (Scatter density (KDE) colouring, Core contour);
architecture: [[architecture/Desktop-Shell]] (Monitoring charts and settings, density bullet).

- **Toggle and colouring.** A "Density (KDE)" checkbox under the scatter's heading. On: the valid cells take
  one of 8 ramp colours by their density (Qt's `kdeLevelForDensity` and `densityRampColor`, light to dark
  blue), the target group is drawn as squares so it stays identifiable, a cell newer than the last estimate
  sits in the sparsest level until the next one. One canvas path per level and shape: no per-point
  element, redraw only when the data, the levels or the view change. Off, the canvas calls are exactly the
  old ones (a pinned digest in `drawDensity.test.ts`). The tooltip carries Qt's text: point count, bandwidth,
  compute ms, refreshed every, core cells and loops, reference label.
- **Core and reference contours.** A solid blue loop around the densest N % (default 90) while KDE is on;
  a dashed orange reference from Pin current or From file… in the Monitoring Settings dialog's new
  "Density (KDE)" group (first group, as in Qt), with Qt's error dialog "No reference contour loaded from
  <path>" + reason, and the file path remembered and reloaded at start-up. Pin and Clear forget the path.
- **Settings and poll.** `useMonitoringDensity` pushes `monitoring_density_set_settings` on every change
  (enabled only while Monitoring is shown) and polls `fetch_monitoring_density` every 500 ms while on, one
  request at a time, nothing set for `unchanged`. A viewer cannot switch it on (the service is one per
  instrument, the command is a control command), so the toggle is in a `ViewerGate` and a viewer sees the
  plain chart. Clear Buffer drops the densities and contours.
- **Preferences.** on/off, bandwidth factor (0.2-5.0, step 0.1), update interval (500-60000 ms, step 500),
  core fraction (5-100 %, step 5) and the reference path persist in one versioned record in the app's
  preference store with `store: "local"`: `persistedState.ts` gained that optional argument because its
  sessionStorage forgets everything when the app closes. Qt's version guard and clamps are kept.
- **Proposed differences from Qt.** (1) The start-up reload keeps the remembered path when the bridge refuses
  the read because an experiment, a review job or a raw recording is busy (Qt drops it on any failure; the
  bridge reads HDF5 only while idle), and drops it, with one log line, when the file is gone. (2) From file…
  is disabled while a run is active. (3) A small ramp key beside the toggle shows the 8 levels while on.
  (4) The text before the first estimate is Qt's (0 points) rather than a "waiting" message.
