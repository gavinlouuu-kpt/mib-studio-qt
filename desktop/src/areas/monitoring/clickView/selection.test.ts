// Click to view, the pure parts (MON-044, 046, 048): stepping through the plotted points, the title, the pin refusals.
import { describe, expect, it } from "vitest";
import type { MonitoringRow } from "../../../bridge";
import { MONITORING_CELLS, MONITORING_CELL_PIN_OUTCOMES as O } from "../../../bridgeContract";
import { measurementsLine, paneTitle, pinCreated, pinRefusal, rowAreaUm2, rowCellKey, stepSelection, stillPlotted } from "./selection";

const row = (frame: number, over: Partial<MonitoringRow> = {}): MonitoringRow => ({
  frame_index: frame, timestamp_ns: frame, valid: true, target_group: false, object_id: 0, object_count: 1, track_id: 7, centroid_x: 0, centroid_y: 0,
  area: 100, deformability: 0.0451, area_ratio: 1.1, ring_ratio: 20.25, youngs_modulus: 2.5, pixel_to_micron: 0.5, start_generation: "1", capture_epoch: "2", ...over,
});
const plotted = [row(10), row(11), row(12), row(13)];

describe("Previous / Next (MON-046)", () => {
  it("walk the plotted points in buffer order and wrap", () => {
    expect(stepSelection(plotted, plotted[1], 1)).toBe(plotted[2]);
    expect(stepSelection(plotted, plotted[1], -1)).toBe(plotted[0]);
    expect(stepSelection(plotted, plotted[3], 1)).toBe(plotted[0]);
    expect(stepSelection(plotted, plotted[0], -1)).toBe(plotted[3]);
  });
  it("find the selection by its whole identity, not by its frame index alone", () => {
    const other = row(11, { capture_epoch: "3" });
    expect(stepSelection(plotted, other, 1)).toBe(plotted[0]); // not among the points: the start of an end
    expect(stepSelection([...plotted, row(11, { object_id: 1 })], plotted[1], 1)).toBe(plotted[2]);
  });
  it("start at an end from a vanished selection or none: Next at the oldest, Previous at the newest", () => {
    const gone = row(99);
    expect(stepSelection(plotted, gone, 1)).toBe(plotted[0]);
    expect(stepSelection(plotted, gone, -1)).toBe(plotted[3]);
    expect(stepSelection(plotted, null, 1)).toBe(plotted[0]);
    expect(stepSelection(plotted, null, -1)).toBe(plotted[3]);
  });
  it("do nothing without plotted points", () => {
    expect(stepSelection([], plotted[0], 1)).toBeNull();
    expect(stepSelection([row(5)], row(5), 1)?.frame_index).toBe(5); // a single point wraps onto itself
  });
});

describe("the title (MON-048)", () => {
  it("is Qt's: frame, calibrated area, deformability, target group", () => {
    expect(paneTitle(row(1234), true)).toBe("Frame 1234 · 25.0 µm² · deformability 0.0451");
    expect(paneTitle(row(1234, { target_group: true }), true)).toBe("Frame 1234 · 25.0 µm² · deformability 0.0451 · target group");
  });
  it("adds 'no longer on the scatter' once the row left the plotted window", () => {
    expect(paneTitle(row(1234), false)).toBe("Frame 1234 · 25.0 µm² · deformability 0.0451 · no longer on the scatter (left the monitoring buffer)");
    expect(stillPlotted(plotted, plotted[2])).toBe(true);
    expect(stillPlotted(plotted, row(99))).toBe(false);
    expect(stillPlotted(plotted, null)).toBe(false);
  });
  it("squares the row's own calibration factor, and says n/a without one", () => {
    expect(rowAreaUm2(row(1, { area: 100, pixel_to_micron: 2 }))).toBe(400);
    expect(paneTitle(row(1, { pixel_to_micron: undefined }), true)).toContain("n/a µm²");
  });
  it("lists the measurements the pane keeps when there is no image", () => {
    expect(measurementsLine(row(1))).toBe("area 100.0 px² (25.00 µm²) · deformability 0.0451 · ring ratio 20.250 · area ratio 1.10 · E 2.50 kPa · object 0 · track 7");
    expect(measurementsLine(row(1, { youngs_modulus: 0 }))).toContain("E n/a");
  });
});

describe("the cell a row names, and the pins", () => {
  it("is the row's identity, '0' for a backend that sends no run or capture", () => {
    expect(rowCellKey(row(5))).toEqual({ start_generation: "1", capture_epoch: "2", frame_index: 5, object_id: 0 });
    expect(rowCellKey(row(5, { start_generation: undefined, capture_epoch: undefined }))).toMatchObject({ start_generation: "0", capture_epoch: "0" });
  });
  it("releases only the pins it made: Pinned and PinnedWithoutImage, not AlreadyPinned or a refusal", () => {
    expect(pinCreated({ ok: true, outcome: O.Pinned })).toBe(true);
    expect(pinCreated({ ok: true, outcome: O.PinnedWithoutImage })).toBe(true);
    expect(pinCreated({ ok: true, outcome: O.AlreadyPinned })).toBe(false);
    expect(pinCreated({ ok: false, outcome: O.NotFound })).toBe(false);
    expect(pinCreated({ ok: true })).toBe(false);
  });
  it("says why a pin was refused in plain words, the backend's own message first", () => {
    expect(pinRefusal({ outcome: O.CaptureMismatch, message: "capture 4 ended: the camera stopped" })).toBe("capture 4 ended: the camera stopped");
    expect(pinRefusal({ outcome: O.CaptureMismatch, message: "" })).toContain("capture changed");
    expect(pinRefusal({ outcome: O.RunMismatch, message: "" })).toContain("another run");
    expect(pinRefusal({ outcome: O.NotFound, message: "" })).toContain("no longer holds");
    expect(pinRefusal({ outcome: O.LimitReached, message: "" })).toContain(`${MONITORING_CELLS.max_pins} pinned cells`);
    expect(pinRefusal({ message: "" })).toContain("did not pin");
  });
});
