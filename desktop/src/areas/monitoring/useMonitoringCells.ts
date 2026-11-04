// Live cell strips (epic #866 MON-052 .. MON-055): the polling and image pulls behind
// CellStrips.tsx. The consumer of bridge.fetchMonitoringCells / fetchMonitoringCellImage.
//
// Rules (AGENT-COMMON.md, the Qt bug #857 pattern is not ported):
//  - nothing runs unless the Monitoring sub-tab is shown (`visible`), as the snapshot polling;
//  - one listing call at a time, every `intervalMs`; the listing is applied only when its revision
//    moved, and images are pulled only for cells the cache does not hold (a crop never changes);
//  - the cache is bounded (both strips plus headroom) and pruned to what the listing names, at most
//    CONCURRENT_FETCHES pulls are in flight, and a queued pull for a cell the newest listing no
//    longer names is skipped;
//  - no per-row or per-frame work between revisions: the component only redraws a thumbnail whose
//    own state or overlay flag changed.
import { useEffect, useState } from "react";
import { bridge } from "../../bridge";
import {
  cellKeyString, monitoringCellCaptureMismatch, type MonitoringCell, type MonitoringCellsListing,
} from "../../monitoringCells";
import {
  CACHE_CELLS, CONCURRENT_FETCHES, CellImageCache, listedKeys, planFetches, type CellImageState, type CellPixels,
} from "./cellStripsModel";

export const CELLS_POLL_MS = 500;

export interface MonitoringCellsView {
  /** The latest listing, or null before the first reply, while the backend has none (`unavailable`) or after an error. */
  listing: MonitoringCellsListing | null;
  /** The backend answered {valid: false} (not initialised). */
  unavailable: boolean;
  /** The last listing or pull error, cleared by the next good listing. */
  error: string | null;
  /** The decoded-pixel states by `cellKeyString`; replaced (new Map) whenever one changes. */
  images: ReadonlyMap<string, CellImageState>;
}

/** The pull of one cell's pixels: the crop, then its mask when the cell has one. */
export async function pullCellPixels(
  cell: MonitoringCell, pull: typeof bridge.fetchMonitoringCellImage = bridge.fetchMonitoringCellImage,
): Promise<CellImageState> {
  try {
    const image = await pull(cell, "image");
    if (!image.valid) return { kind: "none", reason: "the backend no longer holds this cell's image" };
    let mask: Uint8Array | null = null;
    if (cell.image?.has_mask) {
      const m = await pull(cell, "mask");
      // A mask that is gone or of another size is no overlay, never a wrong one.
      if (m.valid && m.width === image.width && m.height === image.height) mask = m.data;
    }
    const pixels: CellPixels = { width: image.width, height: image.height, image: image.data, mask };
    return { kind: "ready", pixels };
  } catch (e) {
    const gone = monitoringCellCaptureMismatch(e);
    return gone !== null
      ? { kind: "none", reason: gone }
      : { kind: "none", reason: `image pull failed: ${e instanceof Error ? e.message : String(e)}`, retryable: true };
  }
}

/**
 * Polls the live cell listing while `visible` and pulls the pixels of new cells. The cache and the
 * pull queue live in the effect; the returned view is replaced only when the listing's revision
 * moved or a pull finished (the renders of one burst of pulls coalesce into one).
 */
export function useMonitoringCells(
  { ready, visible, intervalMs = CELLS_POLL_MS }: { ready: boolean; visible: boolean; intervalMs?: number },
): MonitoringCellsView {
  const [view, setView] = useState<MonitoringCellsView>({ listing: null, unavailable: false, error: null, images: new Map() });

  useEffect(() => {
    if (!ready || !visible) return;
    const cache = new CellImageCache(CACHE_CELLS);
    let disposed = false, polling = false, renderQueued = false;
    let revision: string | null = null;
    let listing: MonitoringCellsListing | null = null;
    let keep: ReadonlySet<string> = new Set();
    let error: string | null = null, unavailable = false;
    const queue: MonitoringCell[] = [];
    let inFlight = 0;
    // A pull that failed for a reason other than "the capture changed" gets one more try (the next
    // poll re-plans it); a cell that fails twice shows why.
    const attempts = new Map<string, number>();
    let retryPending = false;

    const render = () => {
      if (disposed || renderQueued) return;
      renderQueued = true;
      queueMicrotask(() => {
        renderQueued = false;
        if (disposed) return;
        const images = new Map<string, CellImageState>();
        for (const k of cache.keys()) images.set(k, cache.get(k)!);
        setView({ listing, unavailable, error, images });
      });
    };

    const pump = () => {
      while (!disposed && inFlight < CONCURRENT_FETCHES && queue.length > 0) {
        const cell = queue.shift()!;
        const key = cellKeyString(cell);
        if (!keep.has(key) || cache.get(key)?.kind !== "loading") continue; // no longer listed
        inFlight++;
        void pullCellPixels(cell).then(state => {
          inFlight--;
          // A cell the listing dropped while its pull ran is not cached again.
          if (!disposed && keep.has(key)) {
            const tries = (attempts.get(key) ?? 0) + 1;
            if (state.kind === "none" && state.retryable && tries < 2) {
              attempts.set(key, tries);
              cache.delete(key); // the next poll plans it again
              retryPending = true;
            } else {
              cache.set(key, state, keep);
              render();
            }
          }
          pump();
        });
      }
    };

    const poll = async () => {
      if (polling) return;
      polling = true;
      try {
        const reply = await bridge.fetchMonitoringCells();
        if (disposed) return;
        if (!reply.valid) {
          if (!unavailable || error !== null) {
            unavailable = true; error = null; listing = null; revision = null; cache.clear(); keep = new Set(); queue.length = 0;
            render();
          }
          return;
        }
        if (reply.revision === revision && !unavailable && error === null && !retryPending) return; // nothing changed: no work
        unavailable = false; error = null; revision = reply.revision; listing = reply; retryPending = false;
        keep = listedKeys(reply);
        cache.prune(keep);
        for (const k of [...attempts.keys()]) if (!keep.has(k)) attempts.delete(k);
        // The cells the cache has no entry for wait in the queue, newest first. A cell still loading
        // (queued or in flight from an earlier revision) has an entry and is not queued twice.
        for (const cell of planFetches(reply, cache)) {
          cache.set(cellKeyString(cell), { kind: "loading" }, keep);
          queue.push(cell);
        }
        render();
        pump();
      } catch (e) {
        if (!disposed) { error = e instanceof Error ? e.message : String(e); revision = null; render(); }
      } finally { polling = false; }
    };

    void poll();
    const id = window.setInterval(poll, intervalMs);
    return () => { disposed = true; window.clearInterval(id); queue.length = 0; cache.clear(); attempts.clear(); };
  }, [ready, visible, intervalMs]);

  return view;
}
