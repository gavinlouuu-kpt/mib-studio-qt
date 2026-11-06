// Click to view (epic #866, PR E1: MON-044, 046, 047, 048): the pure parts of the selected-cell pane. The Qt
// original is the "Scatter click -> frame pane" section of ExperimentMonitoringTab (selectFrame,
// stepScatterSelection, clearSelection, refreshScatterHighlight). No React, no bridge.
import type { MonitoringRow } from "../../../bridge";
import { MONITORING_CELLS, MONITORING_CELL_PIN_OUTCOMES } from "../../../bridgeContract";
import type { MonitoringCellKeyInput, MonitoringCellPinReply } from "../../../monitoringCells";
import { monitoringRowKey } from "../monitoringChartModel";

/** The identity a row names a cell by (a backend older than ABI 41 sends no run or capture: "0", which the
 *  backend answers as a capture mismatch, with its plain reason). */
export function rowCellKey(row: Pick<MonitoringRow, "start_generation" | "capture_epoch" | "frame_index" | "object_id">): MonitoringCellKeyInput {
  return {
    start_generation: row.start_generation ?? "0",
    capture_epoch: row.capture_epoch ?? "0",
    frame_index: row.frame_index,
    object_id: row.object_id,
  };
}

/** Prev (-1) / Next (+1) through the plotted points in buffer order (oldest to newest), wrapping. From a
 *  selection that is no longer among them they start at an end: Next at the oldest, Prev at the newest (Qt
 *  `stepScatterSelection`). null when there is nothing plotted. */
export function stepSelection(plotted: readonly MonitoringRow[], current: MonitoringRow | null, delta: -1 | 1): MonitoringRow | null {
  const n = plotted.length;
  if (n === 0) return null;
  let from = delta > 0 ? -1 : 0;
  if (current) {
    const key = monitoringRowKey(current);
    const at = plotted.findIndex((r) => monitoringRowKey(r) === key);
    if (at >= 0) from = at;
  }
  return plotted[(((from + delta) % n) + n) % n];
}

/** Whether the selected row is still among the plotted points (the highlight is drawn, the title is plain). */
export function stillPlotted(plotted: readonly MonitoringRow[], current: MonitoringRow | null): boolean {
  if (!current) return false;
  const key = monitoringRowKey(current);
  return plotted.some((r) => monitoringRowKey(r) === key);
}

/** The calibrated area of a row, µm² (its own analysis-time factor squared times its pixel area). */
export const rowAreaUm2 = (row: Pick<MonitoringRow, "area" | "pixel_to_micron">): number => row.area * (row.pixel_to_micron ?? NaN) ** 2;

/** Qt's pane title: "Frame 1234 · 309.8 µm² · deformability 0.0451 · target group", with
 *  " · no longer on the scatter (left the monitoring buffer)" once the row left the plotted window. */
export function paneTitle(row: MonitoringRow, onScatter: boolean): string {
  const area = rowAreaUm2(row);
  let title = `Frame ${row.frame_index} · ${Number.isFinite(area) ? area.toFixed(1) : "n/a"} µm² · deformability ${row.deformability.toFixed(4)}`;
  if (row.target_group) title += " · target group";
  if (!onScatter) title += " · no longer on the scatter (left the monitoring buffer)";
  return title;
}

/** The measurements line of the viewer (what stays when the image is gone). */
export function measurementsLine(row: MonitoringRow): string {
  const num = (v: number, d: number) => (Number.isFinite(v) ? v.toFixed(d) : "n/a");
  const parts = [
    `area ${num(row.area, 1)} px² (${num(rowAreaUm2(row), 2)} µm²)`,
    `deformability ${num(row.deformability, 4)}`,
    `ring ratio ${num(row.ring_ratio, 3)}`,
    `area ratio ${num(row.area_ratio, 2)}`,
    row.youngs_modulus > 0 ? `E ${num(row.youngs_modulus, 2)} kPa` : "E n/a",
    `object ${row.object_id}`,
    `track ${row.track_id}`,
  ];
  return parts.join(" · ");
}

/** The plain sentence for a pin the backend refused (CaptureMismatch, RunMismatch, NotFound, LimitReached) or
 *  did not evaluate; the backend's own message leads when it sent one. */
export function pinRefusal(reply: Pick<MonitoringCellPinReply, "outcome" | "message">): string {
  const O = MONITORING_CELL_PIN_OUTCOMES;
  const fallback = (() => {
    switch (reply.outcome) {
      case O.CaptureMismatch: return "The capture changed since this cell was detected (the camera mode or frame buffer changed), so its image is gone.";
      case O.RunMismatch: return "This cell was detected in another run than the latest one, so its image cannot be kept.";
      case O.NotFound: return "The backend no longer holds this cell.";
      case O.LimitReached: return `The backend already holds its limit of ${MONITORING_CELLS.max_pins} pinned cells: close another cell view or clear the pins, then select again.`;
      default: return "The backend did not pin this cell.";
    }
  })();
  return reply.message ? `${reply.message}` : fallback;
}

/** The pin this client made, as opposed to one that was held before (AlreadyPinned): only what it made is
 *  released when the selection moves, so another holder's pin is never taken away. Pinned (an owned crop) and
 *  PinnedWithoutImage (the measurements only) are the outcomes that create one. */
export const pinCreated = (reply: Pick<MonitoringCellPinReply, "ok" | "outcome">): boolean =>
  reply.ok && (reply.outcome === MONITORING_CELL_PIN_OUTCOMES.Pinned || reply.outcome === MONITORING_CELL_PIN_OUTCOMES.PinnedWithoutImage);
