// The pin / fetch / unpin lifecycle behind the selected-cell pane (epic #866, PR E1: MON-044, MON-048).
//
// A snapshot row only describes a cell; its pixels live in the backend's bounded cell store, which keeps a
// cell the operator pins even after it leaves the monitoring ring (bridge ABI 41/45 `monitoring_cells`,
// at most MONITORING_CELLS.max_pins pins, shared by the instrument). So the pane works like this:
//  - selecting a row PINS it (monitoring_cell_pin, a control command), then pulls its crop and mask by
//    identity (pins are served first); nothing is pulled per poll, only when the selection changes;
//  - when the selection moves, on close (the selection goes to null, by the pane's ✕ or Clear Buffer) and when the
//    studio unmounts, the pin this view made is released (monitoring_cell_unpin); a pin that was already held
//    (AlreadyPinned) is not ours to release;
//  - the hook is called by studio/useStudioController.ts, which is always mounted, NOT by the Monitoring panel: the
//    pin and the pulled image live exactly as long as the selection, so a switch to the Preview sub-tab or to another
//    stage and back neither unpins the cell nor pulls it again (a cell that left the monitoring ring meanwhile would
//    fail to pin again, and Qt keeps its copy of the frame until ✕ or Clear Buffer). Nothing is sent while the
//    Monitoring view is hidden: the effect depends on the selection, the backend's readiness and whether this client
//    controls the instrument, and none of those is a view change;
//  - the pin / unpin calls run strictly one after the other, so an unpin can never overtake the pin it
//    belongs to or a later pin; a selection that moves while its pin is in flight unpins the stale pin as soon
//    as the backend has made it, and one that moves before the pin was sent never sends it;
//  - a client that cannot control the instrument (a viewer in a browser: pin and unpin are control commands)
//    does not pin: it pulls the image of a cell the strips still hold, and says so when the backend has let go;
//  - PinnedWithoutImage shows the measurements and the reason; CaptureMismatch, RunMismatch, NotFound and
//    LimitReached show a plain message.
import { useEffect, useRef, useState } from "react";
import { bridge, type MonitoringRow } from "../../../bridge";
import { MONITORING_CELL_PIN_OUTCOMES } from "../../../bridgeContract";
import type { MonitoringCell } from "../../../monitoringCells";
import type { CellPixels } from "../cellStripsModel";
import { monitoringRowKey } from "../monitoringChartModel";
import { pullKeyPixels } from "../useMonitoringCells";
import { pinCreated, pinRefusal, rowCellKey } from "./selection";

export type CellViewState =
  /** Nothing selected (or the backend not ready). */
  | { phase: "idle" }
  /** The pin or the pull is in flight. */
  | { phase: "loading" }
  | { phase: "ready"; pixels: CellPixels; cell: MonitoringCell | null; pinned: boolean }
  /** Only the measurements: the pin kept no image (PinnedWithoutImage), or no image could be pulled. */
  | { phase: "measurements"; reason: string; cell: MonitoringCell | null }
  /** The backend refused the pin, or the call failed: a plain message. */
  | { phase: "refused"; message: string };

export const CELL_VIEW_IDLE: CellViewState = { phase: "idle" };
const LOADING: CellViewState = { phase: "loading" };

export interface CellViewInput {
  /** The selected row (a copy, so it outlives its place in the monitoring ring), or null. */
  selection: MonitoringRow | null;
  /** The backend is initialised. */
  ready: boolean;
  /** This client controls the instrument: pin and unpin are control commands. */
  canPin: boolean;
  /** One log line per failure that would otherwise vanish (a pin that could not be released). */
  log?: (line: string) => void;
}

const errorText = (e: unknown) => (e instanceof Error ? e.message : String(e));

export function useCellView({ selection, ready, canPin, log }: CellViewInput): CellViewState {
  const [state, setState] = useState<CellViewState>(CELL_VIEW_IDLE);
  // One queue for pin and unpin: they reach the backend in the order they were asked.
  const queue = useRef<Promise<unknown>>(Promise.resolve());
  const logRef = useRef(log);
  logRef.current = log;
  const key = selection ? monitoringRowKey(selection) : null;

  useEffect(() => {
    if (!selection || !ready) {
      setState(CELL_VIEW_IDLE);
      return;
    }
    const identity = rowCellKey(selection);
    const job = { cancelled: false, owned: false, released: false };
    const enqueue = <T,>(task: () => Promise<T>): Promise<T> => {
      const run = queue.current.then(task, task);
      queue.current = run.catch(() => undefined);
      return run;
    };
    /** Give the pin back to the backend (never throws; ok:false is "not pinned any more", a capture change ended it). */
    const unpin = async () => {
      try {
        await bridge.monitoringCellUnpin(identity);
      } catch (e) {
        logRef.current?.(`cell view: the pin of frame ${selection.frame_index} could not be released: ${errorText(e)}`);
      }
    };
    /** Give back the pin this view made (once), in turn. Called when the selection moves, on close and when the studio unmounts. */
    const release = () => {
      if (!job.owned || job.released) return;
      job.released = true;
      void enqueue(unpin);
    };

    setState(LOADING);
    const settle = (next: CellViewState) => { if (!job.cancelled) setState(next); };
    const pull = async (cell: MonitoringCell | null, pinned: boolean) => {
      const pixels = await pullKeyPixels(identity, cell ? cell.image?.has_mask ?? false : null);
      if (pixels.kind === "ready") settle({ phase: "ready", pixels: pixels.pixels, cell, pinned });
      else if (pixels.kind === "none") settle({ phase: "measurements", reason: pixels.reason, cell });
      else settle(LOADING);
    };

    let pinStep: Promise<{ viewOnly: true } | { viewOnly: false; cell: MonitoringCell | null; keptImage: boolean; reason: string } | null>;
    if (canPin) {
      pinStep = enqueue(async () => {
        if (job.cancelled) return null; // moved on before the pin was sent: never send it
        try {
          const reply = await bridge.monitoringCellPin(identity);
          if (pinCreated(reply)) job.owned = true;
          if (job.cancelled) {
            // Moved on while the backend pinned: give the stale pin back now, in this turn, so it is gone before the next pin is asked for.
            if (job.owned && !job.released) { job.released = true; await unpin(); }
            return null;
          }
          if (!reply.ok) { settle({ phase: "refused", message: pinRefusal(reply) }); return null; }
          const withoutImage = reply.outcome === MONITORING_CELL_PIN_OUTCOMES.PinnedWithoutImage || !reply.cell || reply.cell.image === null;
          return { viewOnly: false as const, cell: reply.cell, keptImage: !withoutImage, reason: reply.message || reply.cell?.no_image_reason || "the backend kept no image for this cell" };
        } catch (e) {
          const text = errorText(e);
          // The session said otherwise than this client believed: a viewer's pin is refused, its pull is not.
          if (text.startsWith("VIEWER_ONLY")) return { viewOnly: true as const };
          if (!job.cancelled) settle({ phase: "refused", message: `The cell could not be pinned: ${text}` });
          return null;
        }
      });
    } else pinStep = Promise.resolve({ viewOnly: true as const });

    void pinStep.then(async (step) => {
      if (!step || job.cancelled) return;
      if (step.viewOnly) return pull(null, false);
      if (!step.keptImage) return settle({ phase: "measurements", reason: step.reason, cell: step.cell });
      return pull(step.cell, true);
    }).catch((e) => settle({ phase: "refused", message: `The cell could not be shown: ${errorText(e)}` }));

    return () => {
      job.cancelled = true;
      release();
    };
    // The identity string stands for the row: a poll that hands over the same row again changes nothing.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [key, ready, canPin]);

  return state;
}
