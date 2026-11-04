// What each set (Valid / Invalid) keeps while the other is shown (#867
// REVIEW-018). The Qt tab saves the thumbnail scroll value of the tab it
// leaves and restores the one of the tab it enters (`onTabChanged`:
// validScrollValue_ / invalidScrollValue_), and each set has its own table
// selection; both reset when a file loads. Here both live in one object keyed
// by the open file's key, so a value remembered for another file is never
// used. Pure; unit-tested in review.test.ts.

export type SetName = "valid" | "invalid";

export interface TabMemory {
  fileKey: string;
  /** Vertical scroll of each set's thumbnail grid (px). */
  scroll: Partial<Record<SetName, number>>;
  /** Selected position of each set. */
  selection: Partial<Record<SetName, number>>;
}

export const newTabMemory = (): TabMemory => ({ fileKey: "", scroll: {}, selection: {} });

function forFile(m: TabMemory, fileKey: string): void {
  if (m.fileKey === fileKey) return;
  m.fileKey = fileKey;
  m.scroll = {};
  m.selection = {};
}

/** Where the grid of `set` was left; null = never left (reveal the selection instead). */
export function recallScroll(m: TabMemory, fileKey: string, set: SetName): number | null {
  return m.fileKey === fileKey ? (m.scroll[set] ?? null) : null;
}

export function rememberScroll(m: TabMemory, fileKey: string, set: SetName, top: number): void {
  forFile(m, fileKey);
  m.scroll[set] = Math.max(0, top);
}

/** Called when the operator leaves `from`. The Charts view selects valid-set
 * positions, so a cell chosen there belongs to the Valid set, and the grid
 * position remembered for that set no longer says where the cell is. */
export function leaveTab(m: TabMemory, fileKey: string, from: SetName | "charts", selected: number): void {
  if (selected < 0) return;
  forFile(m, fileKey);
  const set: SetName = from === "charts" ? "valid" : from;
  if (from === "charts" && m.selection[set] !== selected) delete m.scroll[set];
  m.selection[set] = selected;
}

/** The position to select when `set` (of `total` items) is shown: where the
 * operator left it, else the first item (-1 for an empty set). */
export function recallSelection(m: TabMemory, fileKey: string, set: SetName, total: number): number {
  if (total <= 0) return -1;
  const kept = m.fileKey === fileKey ? m.selection[set] : undefined;
  return kept !== undefined && kept >= 0 && kept < total ? kept : 0;
}
