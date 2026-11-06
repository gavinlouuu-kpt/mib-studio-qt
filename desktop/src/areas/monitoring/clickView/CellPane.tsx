// The click-to-view pane and its floating window (epic #866, PR E1: MON-044, 046, 047, 048). The pane is docked
// over the histogram and appears with the first click; the window is the same viewer in a non-modal panel
// inside the app (not a Tauri window) that follows the selection while the charts keep updating. Both step the
// same selection with Previous / Next. The selection and the pin are owned by the caller (LiveMonitoringCharts).
// Nothing here sends a command: the pin lifecycle is useCellView.ts, run by the caller.
import { useRef, useState } from "react";
import type { MonitoringRow } from "../../../bridge";
import { CellViewer } from "./CellViewer";
import { paneTitle } from "./selection";
import type { CellViewState } from "./useCellView";

export interface CellViewProps {
  row: MonitoringRow;
  state: CellViewState;
  /** The row is still among the plotted points (the title says so when it is not). */
  onScatter: boolean;
  overlay: boolean;
  /** Nothing is plotted: Previous / Next have nowhere to go. */
  stepDisabled: boolean;
  onPrev: () => void;
  onNext: () => void;
}

export interface CellPaneProps extends CellViewProps {
  onOpenWindow: () => void;
  /** The ✕ button: hides the pane and clears the selection (Qt `clearSelection`). */
  onClose: () => void;
}

export function CellPane(props: CellPaneProps) {
  const { row, state, onScatter, overlay, stepDisabled, onPrev, onNext, onOpenWindow, onClose } = props;
  const title = paneTitle(row, onScatter);
  return (
    <section className="mon-cell-pane" data-tour="monitoring-cell-pane" aria-label="Selected cell">
      <div className="mon-cell-head">
        <span className="mon-cell-title" data-testid="cell-pane-title">{title}</span>
        <button className="mon-cell-close" data-tour="monitoring-cell-pane-close" title="Close the cell view" aria-label="Close the cell view" onClick={onClose}>
          ✕
        </button>
      </div>
      <CellViewer row={row} state={state} overlay={overlay} />
      <div className="mon-cell-actions">
        <button data-tour="monitoring-cell-prev" onClick={onPrev} disabled={stepDisabled} title="The previous plotted cell (wraps around)">◀ Previous</button>
        <button data-tour="monitoring-cell-next" onClick={onNext} disabled={stepDisabled} title="The next plotted cell (wraps around)">Next ▶</button>
        <button data-tour="monitoring-cell-open-window" onClick={onOpenWindow} title="Open this cell in a separate viewer window">Open in window…</button>
      </div>
    </section>
  );
}

export interface CellWindowProps extends CellViewProps {
  /** Hides the window; the selection stays. */
  onClose: () => void;
}

/** A non-modal floating viewer: no backdrop, the page behind it stays live. Dragged by its title bar. */
export function CellWindow(props: CellWindowProps) {
  const { row, state, onScatter, overlay, stepDisabled, onPrev, onNext, onClose } = props;
  const [at, setAt] = useState<{ left: number; top: number } | null>(null);
  const drag = useRef<{ dx: number; dy: number } | null>(null);
  const frame = useRef<HTMLDivElement>(null);
  const title = paneTitle(row, onScatter);
  return (
    <div
      ref={frame}
      className="mon-cell-window"
      role="dialog"
      aria-modal="false"
      aria-label="Cell viewer window"
      data-tour="monitoring-cell-window"
      style={at ? { left: at.left, top: at.top, right: "auto", bottom: "auto" } : undefined}
    >
      <div
        className="mon-cell-window-bar"
        onPointerDown={(e) => {
          if ((e.target as HTMLElement).closest("button")) return;
          const rect = frame.current!.getBoundingClientRect();
          drag.current = { dx: e.clientX - rect.left, dy: e.clientY - rect.top };
          try { e.currentTarget.setPointerCapture?.(e.pointerId); } catch { /* a pointer that is already gone */ }
        }}
        onPointerMove={(e) => {
          const d = drag.current;
          if (!d) return;
          const w = frame.current?.offsetWidth ?? 0;
          setAt({
            left: Math.min(Math.max(0, e.clientX - d.dx), Math.max(0, window.innerWidth - w)),
            top: Math.min(Math.max(0, e.clientY - d.dy), Math.max(0, window.innerHeight - 40)),
          });
        }}
        onPointerUp={() => { drag.current = null; }}
        onPointerCancel={() => { drag.current = null; }}
      >
        <span className="mon-cell-title" data-testid="cell-window-title">{title}</span>
        <button className="mon-cell-close" data-tour="monitoring-cell-window-close" title="Close the window (the selection stays)" aria-label="Close the cell window" onClick={onClose}>
          ✕
        </button>
      </div>
      <CellViewer row={row} state={state} overlay={overlay} />
      <div className="mon-cell-actions">
        <button data-tour="monitoring-cell-window-prev" onClick={onPrev} disabled={stepDisabled}>◀ Previous</button>
        <button data-tour="monitoring-cell-window-next" onClick={onNext} disabled={stepDisabled}>Next ▶</button>
      </div>
    </div>
  );
}
