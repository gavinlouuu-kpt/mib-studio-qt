// O4 Experiment · Live (#716 A6b): the pure rules. Names, the strip shown, ended pins, the pin
// outcomes in plain words, the scatter (calibrated rows, identities) and its click mapping against the
// kit chart itself, the status-only lifecycle, and which stop each stop action may send.
import { createElement } from "react";
import { renderToStaticMarkup } from "react-dom/server";
import { describe, expect, it } from "vitest";
import { AreaDeformChart } from "@mib/ui";
import { EXPERIMENT_STATES, MONITORING_CELL_PIN_OUTCOMES as P } from "../bridgeContract";
import type { MonitoringRow } from "../bridge";
import type { ExperimentStatus } from "../eventAdapter";
import { decodeMonitoringCells, type MonitoringCellsListing } from "../monitoringCells";
import {
  STRIP_SHOWN,
  cellName,
  cellWhere,
  chartDomain,
  chartPosition,
  droppedPinsNotice,
  endedPins,
  frozenChangeText,
  isEndedPin,
  listingScope,
  liveScatter,
  nearestPoint,
  pinCap,
  pinIdentityRefusal,
  pinNotice,
  pinNoticeStale,
  pinNoticeSubject,
  runCounts,
  scopeChanged,
  statusLifecycle,
  stopAcquisitionReason,
  stopRequestStale,
  stopTarget,
  stripShown,
} from "./liveModel";
import { PLANNED_ROW, type QueueRow } from "./queueModel";

const cell = (frame: string, extra: Record<string, unknown> = {}) => ({
  start_generation: "0", capture_epoch: "3", frame_index: frame, object_id: 1, timestamp_ns: "1",
  valid: true, target_group: false, object_count: 1, track_id: -1, centroid_x: 50, centroid_y: 38,
  bbox: { x: 40, y: 30, w: 20, h: 16 }, area: 600, deformability: 0.05, area_ratio: 1.02, ring_ratio: null,
  youngs_modulus: 1.2, pixel_to_micron: 0.5,
  image: { width: 40, height: 36, crop_x: 30, crop_y: 20, source_width: 512, source_height: 96, has_mask: true, bytes: "2880" },
  no_image_reason: null, ...extra,
});
const listing = (extra: Record<string, unknown> = {}): MonitoringCellsListing => {
  const l = decodeMonitoringCells({
    valid: true, scope_start_generation: "0", ring_start_generation: "0", ring_capture_epoch: "3", revision: "1",
    strip: [], pins: [], last_capture_change: null, stats: {}, caps: { strip_max_cells: 24, max_pins: 16 }, ...extra,
  });
  if (!l.valid) throw new Error("valid listing expected");
  return l;
};
const status = (over: Partial<ExperimentStatus> = {}): ExperimentStatus => ({
  transport_version: 1, valid: true, state: EXPERIMENT_STATES.Idle, start_time_ns: "0", end_time_ns: "0", valid_buffered: "0", invalid_buffered: "0",
  valid_saved: "0", invalid_saved: "0", dropped_valid: "0", dropped_invalid: "0", flushing: false, cancelled: false, output_path: "", message: "",
  start_generation: "0", readiness_generation: "1", capture_generation: "1", persistence_admitted: "0", persistence_committed: "0", persistence_failed: "0",
  terminal: false, finalization_ok: false, completion: 0, completion_reason: "", fault_revision: "0", fault_code: "", fault_message: "", ...over,
} as ExperimentStatus);

describe("names and the strip", () => {
  it("names a live cell by its frame and object", () => {
    expect(cellName({ frame_index: "41118", object_id: 1 })).toBe("F41118·O1");
    expect(cellWhere({ frame_index: "41118", object_id: 1 })).toBe("frame 41 118 · object 1");
    expect(cellWhere({ frame_index: "18446744073709551615", object_id: 0 })).toBe("frame 18 446 744 073 709 551 615 · object 0");
  });

  it("shows the strip newest first, at most STRIP_SHOWN cells", () => {
    const strip = Array.from({ length: 20 }, (_, i) => cell(String(100 + i)));
    const shown = stripShown(listing({ strip }));
    expect(shown).toHaveLength(STRIP_SHOWN);
    expect(shown[0].frame_index).toBe("119");
    expect(shown[STRIP_SHOWN - 1].frame_index).toBe(String(120 - STRIP_SHOWN));
    expect(stripShown(null)).toEqual([]);
  });

  it("tells ended pins (of an earlier capture) from the current capture's", () => {
    const l = listing({ ring_capture_epoch: "4", pins: [
      cell("1", { image: null, no_image_reason: "no image: capture 3 ended (the camera stopped); the pin keeps its measurements" }),
      cell("2", { capture_epoch: "4" }),
    ] });
    expect(l.pins.map((p) => isEndedPin(p, l))).toEqual([true, false]);
    expect(endedPins(l).map((p) => p.frame_index)).toEqual(["1"]);
    expect(pinCap(l)).toBe(16);
    expect(pinCap(null)).toBe(16);
  });
});

describe("the current capture and run (pins, the frozen strip)", () => {
  const change = { capture_epoch: "4", reason: "the camera stopped", pins_ended: 1, pins_dropped: 0, strip_dropped: 3 };
  const key = (capture: string | number | bigint, run: string | number | bigint = "0") => ({ start_generation: run, capture_epoch: capture, frame_index: "9", object_id: 1 });

  it("sends a pin only for the listing's capture, of the latest run or of none (the backend's rule)", () => {
    const l = listing({ ring_capture_epoch: "4", scope_start_generation: "6", ring_start_generation: "6", last_capture_change: change });
    expect(pinIdentityRefusal(key("4", "6"), l)).toBe("");
    expect(pinIdentityRefusal(key(4n, 6n), l)).toBe("");
    expect(pinIdentityRefusal(key("4"), l)).toBe(""); // outside a run: the backend checks no run
    expect(pinIdentityRefusal(key("3", "6"), l)).toBe("It is of capture 3; the current capture is 4 (the camera stopped). Nothing was sent: pick a cell of the current capture.");
    expect(pinIdentityRefusal(key("4", "5"), l)).toBe("It was detected in run 5; the latest run started is run 6, and pins belong to it. Nothing was sent.");
    expect(pinIdentityRefusal(key("3"), listing())).toBe("");
    expect(pinIdentityRefusal(key("3", "2"), listing())).toContain("the latest run started is none");
    expect(pinIdentityRefusal(key("3"), null)).toContain("Nothing was sent");
  });

  it("a frozen strip of another capture or run is dropped, and says which", () => {
    const held = listingScope(listing());
    expect(scopeChanged(held, listing())).toBe(false);
    expect(scopeChanged(held, listing({ revision: "9", strip: [cell("5")] }))).toBe(false); // new cells, same capture
    const camera = listing({ ring_capture_epoch: "4", last_capture_change: change });
    expect(scopeChanged(held, camera)).toBe(true);
    expect(frozenChangeText(held, camera)).toContain("capture 3 ended; capture 4 began (the camera stopped)");
    const run = listing({ scope_start_generation: "7" });
    expect(scopeChanged(held, run)).toBe(true);
    expect(frozenChangeText(held, run)).toContain("run 7 started");
  });
});

describe("pin outcomes in plain words", () => {
  const reply = (outcome: number | undefined, ok: boolean, message: string) => ({ ok, ...(outcome === undefined ? {} : { outcome: outcome as 0 }), message, cell: null });
  it.each([
    [P.Pinned, true, "pinned", "confirmed", "Pinned · F1·O0"],
    [P.PinnedWithoutImage, true, "no image: the frame left the monitoring buffer", "info", "Pinned without an image · F1·O0"],
    [P.AlreadyPinned, true, "already pinned", "info", "Already pinned · F1·O0"],
    [P.RunMismatch, false, "pin refused: frame 1 object 0 was detected outside a run, not in run 5", "warn", "Not pinned · F1·O0 is not of the latest run"],
    [P.NotFound, false, "pin refused: frame 1 object 0 is not held", "warn", "Not pinned · the backend no longer holds F1·O0"],
    [P.LimitReached, false, "pin refused: 16 cells are pinned (the limit); unpin one first", "blocked", "Not pinned · the pin limit is reached"],
    [P.CaptureMismatch, false, "pin refused: frame 1 object 0 is from capture 2, but the current capture is 3 (the camera stopped)", "warn", "Not pinned · F1·O0 is of an earlier capture"],
  ])("outcome %s", (outcome, ok, message, tone, title) => {
    const n = pinNotice("F1·O0", reply(outcome, ok, message));
    expect([n.tone, n.title]).toEqual([tone, title]);
    if (outcome !== P.Pinned && outcome !== P.AlreadyPinned) expect(n.text).toContain(message);
  });
  it("says the limit counts ended pins, and keeps an unknown refusal's message", () => {
    expect(pinNotice("F1·O0", reply(P.LimitReached, false, "x")).text).toContain("Ended pins count toward the limit too");
    expect(pinNotice("F1·O0", reply(undefined, false, "the backend is not initialized"))).toEqual({ tone: "blocked", title: "Not pinned · F1·O0", text: "the backend is not initialized" });
  });
});

describe("the scatter", () => {
  const row = (over: Partial<MonitoringRow>): MonitoringRow => ({
    frame_index: 10, timestamp_ns: 0, valid: true, target_group: false, object_id: 0, object_count: 1, track_id: -1, centroid_x: 1, centroid_y: 1,
    area: 400, deformability: 0.04, area_ratio: 1, ring_ratio: 0.5, youngs_modulus: 1, pixel_to_micron: 0.5, start_generation: "0", capture_epoch: "3", ...over,
  });

  it("plots valid rows in µm² with their own calibration, and keeps each row's identity", () => {
    const s = liveScatter({ valid: true, monitoring_active: true, valid_held: 3, invalid_held: 1, valid_appended: 3, invalid_appended: 1, capacity: 10, latest_timestamp_ns: 0, rows: [
      row({ frame_index: 10 }),
      row({ frame_index: 11, pixel_to_micron: 0 }), // uncalibrated: left out and counted
      row({ frame_index: 12, valid: false }),       // invalid: left out
      row({ frame_index: 13, start_generation: undefined, capture_epoch: undefined }), // an older backend: no identity
    ] });
    expect(s.points.map((p) => [p.row.frame_index, p.area, p.key?.capture_epoch ?? null])).toEqual([[10, 100, "3"], [13, 100, null]]);
    expect(s.points[0].key).toEqual({ start_generation: "0", capture_epoch: "3", frame_index: 10, object_id: 0 });
    expect(s.uncalibrated).toBe(1);
    expect(liveScatter(null)).toEqual({ points: [], uncalibrated: 0 });
  });

  it("keeps the kit's axes unless a point lies beyond them, then widens in whole steps", () => {
    expect(chartDomain([{ area: 120, deformability: 0.05 }])).toEqual({ x: [0, 300], y: [0, 0.2], xTicks: [0, 75, 150, 225, 300], yTicks: [0, 0.05, 0.1, 0.15, 0.2] });
    const d = chartDomain([{ area: 410, deformability: 0.31 }]);
    expect([d.x, d.y]).toEqual([[0, 450], [0, 0.35]]);
  });

  it("maps a point where the kit chart draws it, and a click to the nearest point within 8 px", () => {
    const points = [{ area: 60, deformability: 0.02 }, { area: 150, deformability: 0.1 }, { area: 280, deformability: 0.18 }];
    const domain = chartDomain(points);
    const svg = renderToStaticMarkup(createElement(AreaDeformChart, { points, width: 560, height: 300, xDomain: domain.x, yDomain: domain.y, xTicks: domain.xTicks, yTicks: domain.yTicks }));
    const drawn = [...svg.matchAll(/<circle cx="([\d.]+)" cy="([\d.]+)" r="2"/g)].map((m) => [Number(m[1]), Number(m[2])]);
    expect(drawn).toHaveLength(points.length);
    points.forEach((p, i) => {
      const at = chartPosition(p, domain, 560, 300);
      expect(at.x).toBeCloseTo(drawn[i][0], 6);
      expect(at.y).toBeCloseTo(drawn[i][1], 6);
    });
    const middle = chartPosition(points[1], domain, 560, 300);
    expect(nearestPoint(points, { x: middle.x + 5, y: middle.y - 4 }, domain, 560, 300)).toBe(1);
    expect(nearestPoint(points, { x: middle.x + 9, y: middle.y }, domain, 560, 300)).toBe(-1);
  });
});

describe("the lifecycle from the status alone", () => {
  const states = (s: ExperimentStatus) => statusLifecycle(s)?.map((x) => x.state) ?? null;
  it("is done only where the status confirmed the next step", () => {
    expect(states(status())).toBeNull(); // no run
    expect(states(status({ state: EXPERIMENT_STATES.Starting }))).toEqual(["current", "pending", "pending", "pending"]);
    expect(states(status({ state: EXPERIMENT_STATES.Active }))).toEqual(["done", "current", "pending", "pending"]);
    expect(states(status({ state: EXPERIMENT_STATES.Stopping }))).toEqual(["done", "done", "current", "pending"]);
    expect(states(status({ state: EXPERIMENT_STATES.Active, flushing: true }))).toEqual(["done", "done", "current", "pending"]);
    expect(states(status({ terminal: true, finalization_ok: true }))).toEqual(["done", "done", "done", "done"]);
    const failed = statusLifecycle(status({ state: EXPERIMENT_STATES.Failed, terminal: true }))!;
    expect(failed.map((x) => x.state)).toEqual(["done", "done", "done", "failed"]);
    expect(failed[3].flag).toEqual({ tone: "blocked", text: "Failed" });
    expect(statusLifecycle(status({ valid: false }))).toBeNull();
  });
  it("counts as the backend reports, with the pending records settled against what was admitted", () => {
    expect(runCounts(status({ valid_saved: "63004", persistence_admitted: "63200", persistence_committed: "63004", dropped_valid: "2", dropped_invalid: "1" })))
      .toEqual({ saved: "63 004", invalidSaved: "0", pending: "193", dropped: "3", failed: "0" });
    expect(runCounts(status({ valid: false }))).toBeNull();
  });
});

describe("the stops", () => {
  const active = status({ state: EXPERIMENT_STATES.Active, start_generation: "5", output_path: "/data/s1.h5" });
  const running: QueueRow = { ...PLANNED_ROW, phase: "running", attempts: [{ runIndex: 1, outcome: "started", path: "/data/s1.h5", generationBefore: "4", generation: "5" }] };
  const base = { controlHeld: true, pending: false, status: active, queue: null };

  it("stops the run the status shows active, by its generation; nothing without one", () => {
    expect(stopTarget(base)).toEqual({ kind: "run", generation: "5" });
    expect(stopTarget({ ...base, status: status() })).toEqual({ kind: "none", reason: "No run is active." });
    expect(stopTarget({ ...base, status: status({ state: EXPERIMENT_STATES.Stopping, start_generation: "5" })}).kind).toBe("none");
    expect(stopTarget({ ...base, status: null }).kind).toBe("none");
  });
  it("needs control, and sends no second stop while one is on its way", () => {
    expect(stopTarget({ ...base, controlHeld: false })).toEqual({ kind: "none", reason: "This client does not control the instrument: take control to stop a run." });
    expect(stopTarget({ ...base, pending: true }).kind).toBe("none");
  });
  it("in a queue, stops only the queue's own row, and never while it is paused", () => {
    expect(stopTarget({ ...base, queue: { row: running, paused: false } })).toEqual({ kind: "queue" });
    expect(stopTarget({ ...base, queue: { row: running, paused: true } }).kind).toBe("none");
    const other = status({ state: EXPERIMENT_STATES.Active, start_generation: "6", output_path: "/data/theirs.h5" });
    expect(stopTarget({ ...base, status: other, queue: { row: running, paused: false } })).toEqual({ kind: "none", reason: "Waiting for the status to show the queue's run active." });
    expect(stopTarget({ ...base, status: other, queue: { row: PLANNED_ROW, paused: false } }))
      .toEqual({ kind: "none", reason: "The active run is not this queue's: the queue never stops another run. Stop it in the classic view." });
  });
  it("stops the camera only with control, while it runs, and never under a run", () => {
    expect(stopAcquisitionReason(true, true, false)).toBe("");
    expect(stopAcquisitionReason(true, false, false)).toBe("The camera is not running.");
    expect(stopAcquisitionReason(true, true, true)).toContain("A run is active");
    expect(stopAcquisitionReason(false, true, false)).toContain("does not control the instrument");
  });
});

describe("stale notices and cleared pins (#763)", () => {
  const pinned = (over: Record<string, unknown> = {}) =>
    ({ ok: true, outcome: P.Pinned, message: "pinned", revision: "9", cell: listing({ pins: [cell("7")] }).pins[0], ...over });

  it("F13: a pin's notice names the pin only when the reply says it exists", () => {
    expect(pinNoticeSubject(pinned() as never)).toEqual({ key: "0:3:7:1", epoch: "3", revision: "9" });
    expect(pinNoticeSubject(pinned({ ok: false, outcome: P.LimitReached, cell: null }) as never)).toBeNull();
    expect(pinNoticeSubject(pinned({ revision: undefined }) as never)).toEqual({ key: "0:3:7:1", epoch: "3" });
  });

  it("F13: Pinned · F… is stale once its capture ended, or a listing as new as the reply lacks it; never on an older listing", () => {
    const about = { key: "0:3:7:1", epoch: "3", revision: "9" };
    expect(pinNoticeStale(about, null)).toBe(false);
    expect(pinNoticeStale(about, listing({ revision: "5" }))).toBe(false); // read before the pin's reply
    expect(pinNoticeStale(about, listing({ revision: "9", pins: [cell("7")] }))).toBe(false);
    expect(pinNoticeStale(about, listing({ revision: "10" }))).toBe(true); // unpinned since
    expect(pinNoticeStale(about, listing({ ring_capture_epoch: "4", revision: "10", pins: [cell("7", { image: null, no_image_reason: "no image: capture 3 ended" })] }))).toBe(true); // its capture ended
    expect(pinNoticeStale({ key: "0:3:7:1", epoch: "3" }, listing({ revision: "99" }))).toBe(false); // no revision: only a capture change clears it
  });

  it("F13: Stop requested is stale once its run is terminal or another run is shown", () => {
    expect(stopRequestStale("5", null)).toBe(false);
    expect(stopRequestStale("5", status({ state: EXPERIMENT_STATES.Active, start_generation: "5" }))).toBe(false);
    expect(stopRequestStale("5", status({ state: EXPERIMENT_STATES.Stopping, flushing: true, start_generation: "5" }))).toBe(false);
    expect(stopRequestStale("5", status({ terminal: true, finalization_ok: true, start_generation: "5" }))).toBe(true);
    expect(stopRequestStale("5", status({ state: EXPERIMENT_STATES.Active, start_generation: "6" }))).toBe(true);
    expect(stopRequestStale("5", status({ valid: false }))).toBe(false);
  });

  it("F15: says a run start's cleared pins while that capture is current", () => {
    const change = (n: number, epoch = "8") => ({ capture_epoch: epoch, reason: "a run started", pins_ended: 0, pins_dropped: n, strip_dropped: 0 });
    const l = listing({ scope_start_generation: "7", ring_capture_epoch: "8", last_capture_change: change(1) });
    expect(droppedPinsNotice(l)).toEqual({ epoch: "8", tone: "info", title: "1 pin cleared · run generation 7 started",
      text: "Pins belong to the latest run started, so the backend cleared the pin when this run started. Pin cells of this run to keep them." });
    expect(droppedPinsNotice(listing({ scope_start_generation: "7", ring_capture_epoch: "8", last_capture_change: change(3) }))!.title).toBe("3 pins cleared · run generation 7 started");
    expect(droppedPinsNotice(listing({ ring_capture_epoch: "9", last_capture_change: change(1, "8") }))).toBeNull(); // a later capture
    expect(droppedPinsNotice(listing({ ring_capture_epoch: "8", last_capture_change: change(0) }))).toBeNull();
    expect(droppedPinsNotice(listing())).toBeNull();
    expect(droppedPinsNotice(null)).toBeNull();
  });
});
