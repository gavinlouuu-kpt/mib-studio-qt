// A two-pane split with a draggable handle (the Qt tab's QSplitter, #867
// REVIEW-039). The first pane's share persists under `storageKey`; the pointer
// drag saves once, on release (Qt saves once, not per pixel), the keyboard
// (arrows along the split, Shift for bigger steps) on each press.
import { useRef, useState, type KeyboardEvent, type PointerEvent, type ReactNode } from "react";
import { clampShare, dragShare, loadSplit, saveSplit, SPLITTER_PX } from "./splitter";

const KEY_STEP = 0.02;
const KEY_STEP_BIG = 0.1;

export interface SplitPaneProps {
  /** "row": first | second side by side; "column": first over second. */
  direction: "row" | "column";
  storageKey: string;
  defaultShare: number;
  /** Minimum size of each pane along the split (px). */
  minFirst: number;
  minSecond: number;
  /** Accessible name of the handle. */
  label: string;
  first: ReactNode;
  second: ReactNode;
  tour?: string;
}

export function SplitPane(props: SplitPaneProps) {
  const { direction, storageKey, defaultShare, minFirst, minSecond, label, first, second, tour } = props;
  const row = direction === "row";
  const box = useRef<HTMLDivElement>(null);
  const [share, setShare] = useState(() => loadSplit(storageKey, defaultShare));
  const [dragging, setDragging] = useState(false);
  const latest = useRef(share);

  const available = () => {
    const r = box.current?.getBoundingClientRect();
    return Math.max(0, (row ? r?.width : r?.height) ?? 0) - SPLITTER_PX;
  };
  const move = (next: number) => {
    latest.current = next;
    setShare(next);
  };

  const onPointerDown = (e: PointerEvent<HTMLDivElement>) => {
    if (e.button !== 0) return;
    e.preventDefault();
    e.stopPropagation();
    e.currentTarget.setPointerCapture?.(e.pointerId);
    setDragging(true);
  };
  const onPointerMove = (e: PointerEvent<HTMLDivElement>) => {
    const r = box.current?.getBoundingClientRect();
    const room = available();
    if (!dragging || !r || room <= 0) return;
    move(dragShare(row ? e.clientX - r.left : e.clientY - r.top, room, minFirst, minSecond));
  };
  const end = () => {
    if (!dragging) return;
    setDragging(false);
    saveSplit(storageKey, latest.current);
  };
  const onKeyDown = (e: KeyboardEvent<HTMLDivElement>) => {
    const keys = row ? ["ArrowLeft", "ArrowRight"] : ["ArrowUp", "ArrowDown"];
    const at = keys.indexOf(e.key);
    if (at < 0) return;
    // The Charts view steps the selected cell with the arrows; not from here.
    e.preventDefault();
    e.stopPropagation();
    const next = clampShare(latest.current + (at === 0 ? -1 : 1) * (e.shiftKey ? KEY_STEP_BIG : KEY_STEP), available(), minFirst, minSecond);
    move(next);
    saveSplit(storageKey, next);
  };

  const size = row ? "minWidth" : "minHeight";
  return (
    <div ref={box} className={`split ${direction}`} data-tour={tour}>
      <div className="split-first" style={{ flex: `${share} 1 0`, [size]: minFirst }}>
        {first}
      </div>
      <div
        className={`splitter ${direction}${dragging ? " dragging" : ""}`}
        role="separator"
        aria-orientation={row ? "vertical" : "horizontal"}
        aria-label={label}
        aria-valuemin={5}
        aria-valuemax={95}
        aria-valuenow={Math.round(share * 100)}
        tabIndex={0}
        onPointerDown={onPointerDown}
        onPointerMove={onPointerMove}
        onPointerUp={end}
        onPointerCancel={end}
        onKeyDown={onKeyDown}
      />
      <div className="split-second" style={{ flex: `${1 - share} 1 0`, [size]: minSecond }}>
        {second}
      </div>
    </div>
  );
}
