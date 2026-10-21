// Pure model of the workspace's Experiment · Live view (O4, #716 A6b): what the view shows from the
// live cell store (bridge ABI 41, monitoring_cells), the monitoring snapshot and the experiment
// status, and which stop each stop action may send. No React, transport or canvas imports.
//
// - Cells. The backend owns the copies: the strip (the newest detected cells of the current capture)
//   and the pins (bounded; a pin survives ring eviction). A live cell has no record id yet (the
//   writer assigns those), so the view names it by its frame and object.
// - Ended pins. At a capture change the strip is cleared and the pins of the capture that ended keep
//   their measurements only, with the reason; they still count toward the pin cap until unpinned.
// - The scatter is the monitoring snapshot's valid, calibrated rows (the classic chart's rule:
//   area in µm² with each row's own analysis-time px→µm; rows without it are left out and counted).
//   A click pins the nearest point by the row's identity; the backend decides the outcome.
// - Lifecycle. A queue row follows the queue's own lifecycle (queueModel.ts). Any other run is
//   followed from the experiment status alone: a step is done only when the status confirmed it.
// - Stops. "Stop & finalize run" stops one named run: the queue's own row (through the queue), or the
//   run the status shows active (its start_generation as expectedStartGeneration, ABI 40). "Stop
//   acquisition" stops the camera, never during a run (the classic Stop Camera rule).

import { EXPERIMENT_STATES, MONITORING_CELLS, MONITORING_CELL_PIN_OUTCOMES as P } from "../bridgeContract";
import type { MonitoringRow, MonitoringSnapshot } from "../bridge";
import { monitoringPlotData } from "../components/MonitoringCharts";
import type { ExperimentStatus } from "../eventAdapter";
import type { MonitoringCell, MonitoringCellKeyInput, MonitoringCellPinReply, MonitoringCellsListing } from "../monitoringCells";
import { ownRunActive, type LifecycleStep, type QueueRow } from "./queueModel";

export type LiveTone = "confirmed" | "info" | "warn" | "blocked" | "unknown";
export interface LiveNotice { tone: LiveTone; title: string; text: string }

/** At most this many strip cells are shown and have their images pulled (newest first). */
export const STRIP_SHOWN = 12;

const grouped = (s: string | number | bigint): string => {
  try { return BigInt(s).toLocaleString("en-US").replace(/,/g, " "); } catch { return String(s); }
};

/** A live cell's short name: its frame and object (live rows have no record id). */
export const cellName = (key: { frame_index: string | number | bigint; object_id: number }): string => `F${key.frame_index}·O${key.object_id}`;

/** "frame 41 118 · object 1". */
export const cellWhere = (key: { frame_index: string | number | bigint; object_id: number }): string =>
  `frame ${grouped(key.frame_index)} · object ${key.object_id}`;

/** The strip as shown: newest first (the listing is oldest to newest), at most STRIP_SHOWN. */
export const stripShown = (l: MonitoringCellsListing | null): MonitoringCell[] => (l ? [...l.strip].reverse().slice(0, STRIP_SHOWN) : []);

/** A pin of a capture that has ended: its measurements only, until it is unpinned. */
export const isEndedPin = (pin: MonitoringCell, l: MonitoringCellsListing): boolean => pin.capture_epoch !== l.ring_capture_epoch;
export const endedPins = (l: MonitoringCellsListing | null): MonitoringCell[] => (l ? l.pins.filter((p) => isEndedPin(p, l)) : []);

/** The capture and run a listing names: a cell of another is never sent (pin) or shown (a frozen strip). */
export interface CellScope { epoch: string; scope: string }
export const listingScope = (l: MonitoringCellsListing): CellScope => ({ epoch: l.ring_capture_epoch, scope: l.scope_start_generation });
/** A listing of another capture or another latest run than `s`. */
export const scopeChanged = (s: CellScope, l: MonitoringCellsListing): boolean => s.epoch !== l.ring_capture_epoch || s.scope !== l.scope_start_generation;

/** The capture change that began the listing's capture, in plain words ("" when it does not say). */
const changeReason = (l: MonitoringCellsListing): string =>
  l.last_capture_change && l.last_capture_change.capture_epoch === l.ring_capture_epoch ? ` (${l.last_capture_change.reason})` : "";

/** Why a cell identity is not sent as a pin ("" when it may be): it must be of the listing's capture,
 *  and of the latest run started or of none (the backend's own rule; it would refuse with
 *  CaptureMismatch or RunMismatch). Without a listing the current capture is not known. */
export function pinIdentityRefusal(key: MonitoringCellKeyInput, l: MonitoringCellsListing | null): string {
  if (!l) return "The live cells have not been read yet, so the current capture is not known. Nothing was sent.";
  const epoch = String(key.capture_epoch), run = String(key.start_generation);
  if (epoch !== l.ring_capture_epoch) {
    return `It is of capture ${epoch}; the current capture is ${l.ring_capture_epoch}${changeReason(l)}. Nothing was sent: pick a cell of the current capture.`;
  }
  if (run !== "0" && run !== l.scope_start_generation) {
    return `It was detected in run ${run}; the latest run started is ${l.scope_start_generation === "0" ? "none" : `run ${l.scope_start_generation}`}, and pins belong to it. Nothing was sent.`;
  }
  return "";
}

/** The frozen strip's cells were of an earlier capture or run: they are dropped, never shown or pinned. */
export function frozenChangeText(held: CellScope, l: MonitoringCellsListing): string {
  const what = held.epoch !== l.ring_capture_epoch
    ? `capture ${held.epoch} ended; capture ${l.ring_capture_epoch} began${changeReason(l)}`
    : `run ${l.scope_start_generation} started`;
  return `The frozen cells are dropped: ${what}. Their crops were of the earlier capture, so they are not shown or pinned. Resume display to follow the strip of the current capture.`;
}

/** The pin cap as the listing reports it (the contract's when absent). */
export const pinCap = (l: MonitoringCellsListing | null): number => {
  const cap = l?.caps.max_pins;
  return typeof cap === "number" && cap > 0 ? cap : MONITORING_CELLS.max_pins;
};

/** What a pin's reply means, in plain words. The backend's message is kept as it is. */
export function pinNotice(name: string, reply: MonitoringCellPinReply): LiveNotice {
  const m = reply.message;
  switch (reply.outcome) {
    case P.Pinned: return { tone: "confirmed", title: `Pinned · ${name}`, text: "The backend keeps its own copy of the crop and mask: it survives ring eviction until you unpin it." };
    case P.PinnedWithoutImage: return { tone: "info", title: `Pinned without an image · ${name}`, text: `${m}. Its measurements are kept; no image is shown in its place.` };
    case P.AlreadyPinned: return { tone: "info", title: `Already pinned · ${name}`, text: "It is selected below." };
    case P.RunMismatch: return { tone: "warn", title: `Not pinned · ${name} is not of the latest run`, text: `${m}. Pins belong to the latest run started.` };
    case P.NotFound: return { tone: "warn", title: `Not pinned · the backend no longer holds ${name}`, text: m };
    case P.LimitReached: return { tone: "blocked", title: "Not pinned · the pin limit is reached", text: `${m}. Ended pins count toward the limit too: clear them or unpin a cell, then pin again.` };
    case P.CaptureMismatch: return { tone: "warn", title: `Not pinned · ${name} is of an earlier capture`, text: `${m}. Pick a cell of the current capture.` };
    default: return reply.ok
      ? { tone: "info", title: `Pinned · ${name}`, text: m }
      : { tone: "blocked", title: `Not pinned · ${name}`, text: m || "the backend did not evaluate the pin" };
  }
}

// ---- the scatter ---------------------------------------------------------------------------------

export interface LivePoint {
  /** µm², with the row's own analysis-time px→µm. */
  area: number;
  deformability: number;
  row: MonitoringRow;
  /** The row's identity, or null for a row without one (an older backend): such a point cannot be pinned. */
  key: MonitoringCellKeyInput | null;
}

export interface LiveScatter {
  points: LivePoint[];
  /** Valid rows left out because they have no analysis-time calibration. */
  uncalibrated: number;
}

export function liveScatter(snapshot: MonitoringSnapshot | null): LiveScatter {
  if (!snapshot?.valid) return { points: [], uncalibrated: 0 };
  const data = monitoringPlotData(snapshot.rows);
  const points = data.scatter.map((row) => ({
    area: row.area,
    deformability: row.deformability,
    row,
    key: row.start_generation !== undefined && row.capture_epoch !== undefined
      ? { start_generation: row.start_generation, capture_epoch: row.capture_epoch, frame_index: row.frame_index, object_id: row.object_id }
      : null,
  }));
  return { points, uncalibrated: data.uncalibrated };
}

export interface ChartDomain { x: [number, number]; y: [number, number]; xTicks: number[]; yTicks: number[] }

const ticks = (max: number) => [0, 1, 2, 3, 4].map((i) => Number(((max * i) / 4).toFixed(6)));

/** The kit chart's domain (0–300 µm², 0–0.20), widened in whole steps when a point lies beyond it. */
export function chartDomain(points: readonly { area: number; deformability: number }[]): ChartDomain {
  let area = 0, def = 0;
  for (const p of points) { area = Math.max(area, p.area); def = Math.max(def, p.deformability); }
  const x = Math.max(300, Math.ceil(area / 75) * 75), y = Math.max(0.2, Math.ceil(def / 0.05) * 0.05);
  return { x: [0, x], y: [0, Number(y.toFixed(6))], xTicks: ticks(x), yTicks: ticks(y) };
}

/** The kit AreaDeformChart's plot margins (desktop/ui AreaDeformChart.tsx). The click layer maps a
 *  click to the nearest point with them until the kit offers point selection (requested on #715);
 *  liveModel.test.ts renders the kit chart and checks each point is where chartPosition puts it. */
export const CHART_MARGIN = { top: 28, right: 40, bottom: 44, left: 52 } as const;

export function chartPosition(p: { area: number; deformability: number }, d: ChartDomain, width: number, height: number): { x: number; y: number } {
  const w = width - CHART_MARGIN.left - CHART_MARGIN.right, h = height - CHART_MARGIN.top - CHART_MARGIN.bottom;
  return {
    x: CHART_MARGIN.left + ((p.area - d.x[0]) / (d.x[1] - d.x[0])) * w,
    y: CHART_MARGIN.top + (1 - (p.deformability - d.y[0]) / (d.y[1] - d.y[0])) * h,
  };
}

/** The point nearest a click (chart pixels), within `radius`; -1 when none is that close. */
export function nearestPoint(points: readonly { area: number; deformability: number }[], click: { x: number; y: number },
  d: ChartDomain, width: number, height: number, radius = 8): number {
  let best = -1, bestDistance = radius * radius;
  points.forEach((p, i) => {
    const at = chartPosition(p, d, width, height);
    const distance = (at.x - click.x) ** 2 + (at.y - click.y) ** 2;
    if (distance <= bestDistance) { best = i; bestDistance = distance; }
  });
  return best;
}

/** A cell's position on the scatter (µm²), when it has a calibration and finite measurements. */
export function cellOnChart(c: MonitoringCell): { area: number; deformability: number } | null {
  if (c.area === null || c.deformability === null || c.pixel_to_micron === null || c.pixel_to_micron <= 0) return null;
  const area = c.area * c.pixel_to_micron * c.pixel_to_micron;
  return Number.isFinite(area) ? { area, deformability: c.deformability } : null;
}

// ---- the run the view follows --------------------------------------------------------------------

const STEP_LABELS = { starting: "Starting", running: "Running", stopping: "Stopping · drain", finalized: "Finalized" } as const;
const STATUS_STEPS = ["starting", "running", "stopping", "finalized"] as const;

/** A run's lifecycle from the experiment status alone (a single-sample run, or a run started outside
 *  this plan): Starting → Running → Stopping · drain → Finalized. A step is done only once the status
 *  reports the next one; null when the status shows no run. */
export function statusLifecycle(s: ExperimentStatus | null): LifecycleStep[] | null {
  if (!s?.valid) return null;
  const failed = s.state === EXPERIMENT_STATES.Failed || (s.terminal && (!s.finalization_ok || !!s.fault_code));
  let index: number, state: LifecycleStep["state"] = "current";
  if (failed) { index = 3; state = "failed"; }
  else if (s.terminal) { index = 4; }
  else if (s.state === EXPERIMENT_STATES.Stopping || s.flushing) index = 2;
  else if (s.state === EXPERIMENT_STATES.Active) index = 1;
  else if (s.state === EXPERIMENT_STATES.Starting) index = 0;
  else return null;
  return STATUS_STEPS.map((key, i) => {
    const step: LifecycleStep = { key, label: STEP_LABELS[key], state: i < index ? "done" : i === index ? state : "pending" };
    if (i === index && failed) step.flag = { tone: "blocked", text: "Failed" };
    return step;
  });
}

/** A status that shows a run in progress (not terminal): Starting, Active or Stopping. */
export const runInProgress = (s: ExperimentStatus | null): boolean =>
  !!s?.valid && !s.terminal && (s.state === EXPERIMENT_STATES.Starting || s.state === EXPERIMENT_STATES.Active || s.state === EXPERIMENT_STATES.Stopping || s.flushing);

export type StopTarget =
  | { kind: "queue" }
  | { kind: "run"; generation: string }
  | { kind: "none"; reason: string };

export interface StopInput {
  controlHeld: boolean;
  /** A stop (or a start) is already on its way. */
  pending: boolean;
  status: ExperimentStatus | null;
  /** The sample queue, when the plan is a queue that has begun: its current row, and whether it is paused. */
  queue: { row: QueueRow | null; paused: boolean } | null;
}

/** Which run "Stop & finalize run" stops now, or why it stops none. */
export function stopTarget({ controlHeld, pending, status, queue }: StopInput): StopTarget {
  if (!controlHeld) return { kind: "none", reason: "This client does not control the instrument: take control to stop a run." };
  if (pending) return { kind: "none", reason: "A request is on its way." };
  if (queue) {
    if (queue.paused) return { kind: "none", reason: "The sample queue is paused: resume it first. Resuming sends nothing." };
    if (queue.row && ownRunActive(queue.row, status)) return { kind: "queue" };
    if (queue.row?.phase === "stopping") return { kind: "none", reason: "The queue's run is stopping: draining and finalizing." };
    if (queue.row?.phase === "running") return { kind: "none", reason: "Waiting for the status to show the queue's run active." };
    if (runInProgress(status)) return { kind: "none", reason: "The active run is not this queue's: the queue never stops another run. Stop it in the classic view." };
    return { kind: "none", reason: "No run of the queue is active." };
  }
  if (!status?.valid) return { kind: "none", reason: "The run status has not been read yet." };
  if (status.state === EXPERIMENT_STATES.Active && !status.terminal && !status.flushing) return { kind: "run", generation: status.start_generation };
  if (runInProgress(status)) return { kind: "none", reason: "The run is starting or stopping: wait for the status." };
  return { kind: "none", reason: "No run is active." };
}

/** Why "Stop acquisition" (the camera) is unavailable; "" when it may stop the camera. */
export function stopAcquisitionReason(controlHeld: boolean, running: boolean, experimentActive: boolean): string {
  if (!controlHeld) return "This client does not control the instrument: take control to stop the camera.";
  if (!running) return "The camera is not running.";
  if (experimentActive) return "A run is active: stop and finalize it first. The camera never stops under a run.";
  return "";
}

/** The run's counts for the status card, as the backend reports them (decimal strings). */
export function runCounts(s: ExperimentStatus | null): { saved: string; invalidSaved: string; pending: string; dropped: string; failed: string } | null {
  if (!s?.valid) return null;
  const n = (v: string) => { try { return BigInt(v); } catch { return 0n; } };
  const unsettled = n(s.persistence_admitted) - n(s.persistence_committed) - n(s.persistence_failed) - n(s.dropped_valid) - n(s.dropped_invalid);
  return {
    saved: grouped(s.valid_saved),
    invalidSaved: grouped(s.invalid_saved),
    pending: grouped(String(unsettled > 0n ? unsettled : 0n)),
    dropped: grouped(String(n(s.dropped_valid) + n(s.dropped_invalid))),
    failed: grouped(s.persistence_failed),
  };
}
