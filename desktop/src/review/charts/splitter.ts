// Charts view splitters (#867 REVIEW-039): scatter | (frame pane over
// histogram) in two draggable splits whose positions persist, as the Qt tab's
// QSplitters do (`Review/ChartsSplitter`, `Review/ChartsRightSplitter`, saved
// from `setupChartsLayout`). QSettings → localStorage under the module's
// `yofo.review.*` keys; the value is the first pane's share of the space. Pure;
// unit-tested in review.test.ts.

export const CHARTS_SPLITTER_KEY = "yofo.review.chartsSplitter";
export const CHARTS_RIGHT_SPLITTER_KEY = "yofo.review.chartsRightSplitter";

// Qt `setSizes`: {600, 400} left | right, {550, 300} pane over histogram.
export const CHARTS_SPLIT_DEFAULT = 600 / (600 + 400);
export const CHARTS_RIGHT_SPLIT_DEFAULT = 550 / (550 + 300);

// Qt minimums: scatter 420 px, frame pane 320 px wide (children not collapsible).
export const SCATTER_MIN_WIDTH = 420;
export const PANE_MIN_WIDTH = 320;
export const PANE_MIN_HEIGHT = 140;
export const HISTOGRAM_MIN_HEIGHT = 140;

/** Thickness of the drag handle (px). */
export const SPLITTER_PX = 8;

const MIN_SHARE = 0.05;
const MAX_SHARE = 0.95;

/**
 * Keep the first pane's share inside what both panes' minimum sizes allow in
 * `available` px (the container minus the handle). When the container is
 * smaller than both minimums together, the minimums are shared out as they are.
 */
export function clampShare(share: number, available: number, minFirst: number, minSecond: number): number {
  const s = Number.isFinite(share) ? share : 0.5;
  if (!(available > 0)) return Math.min(MAX_SHARE, Math.max(MIN_SHARE, s));
  const lo = minFirst / available;
  const hi = 1 - minSecond / available;
  if (lo > hi) return minFirst / (minFirst + minSecond);
  return Math.min(hi, Math.max(lo, s));
}

/** The first pane's share when the handle is dragged to `pointer` px from the container's start. */
export function dragShare(pointer: number, available: number, minFirst: number, minSecond: number): number {
  return clampShare((pointer - SPLITTER_PX / 2) / available, available, minFirst, minSecond);
}

function safeStorage(): Storage | null {
  try {
    return typeof localStorage === "undefined" ? null : localStorage;
  } catch {
    return null;
  }
}

/** The stored share, or `fallback` when none is stored or it is not a usable number. */
export function loadSplit(key: string, fallback: number, storage: Pick<Storage, "getItem"> | null = safeStorage()): number {
  try {
    const raw = storage?.getItem(key);
    const v = raw === null || raw === undefined || raw === "" ? NaN : Number(raw);
    return Number.isFinite(v) && v >= MIN_SHARE && v <= MAX_SHARE ? v : fallback;
  } catch {
    return fallback;
  }
}

export function saveSplit(key: string, share: number, storage: Pick<Storage, "setItem"> | null = safeStorage()): void {
  try {
    storage?.setItem(key, String(Math.round(share * 10000) / 10000));
  } catch {
    // Storage unavailable: the position lasts for this session only.
  }
}
