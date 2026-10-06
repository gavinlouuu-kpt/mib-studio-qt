// Click-vs-drag state machine for the review charts — the behaviour of the
// Qt ZoomableChartView: a left or middle press is a click until the pointer
// travels DRAG_THRESHOLD_PX (Manhattan), then it pans; a release that never
// became a drag is a click. No DOM: the component feeds pointer events in
// and acts on the returned action. Unit-tested in charts.test.ts.

export const DRAG_THRESHOLD_PX = 10;

export type GestureAction =
  | { kind: "none" }
  | { kind: "pan"; dx: number; dy: number }
  | { kind: "click"; x: number; y: number; button: number }
  | { kind: "hover"; x: number; y: number };

const NONE: GestureAction = { kind: "none" };

export class GestureTracker {
  private pending = false;
  private panning = false;
  private button = -1;
  private pressX = 0;
  private pressY = 0;
  private lastX = 0;
  private lastY = 0;

  get active(): boolean {
    return this.pending || this.panning;
  }

  get isPanning(): boolean {
    return this.panning;
  }

  /** button: DOM MouseEvent.button (0 left, 1 middle, 2 right). */
  down(x: number, y: number, button: number): GestureAction {
    if (button !== 0 && button !== 1) return NONE;
    this.pending = true;
    this.panning = false;
    this.button = button;
    this.pressX = this.lastX = x;
    this.pressY = this.lastY = y;
    return NONE;
  }

  /** `buttons`: DOM MouseEvent.buttons bitmask at the time of the move. */
  move(x: number, y: number, buttons: number): GestureAction {
    // The release went elsewhere (a menu, another window): a move with the
    // pressed button up ends the gesture.
    if (this.active && !(buttons & buttonMask(this.button))) this.cancel();
    if (this.pending) {
      if (Math.abs(x - this.pressX) + Math.abs(y - this.pressY) < DRAG_THRESHOLD_PX) return NONE;
      this.pending = false;
      this.panning = true;
      // The pan starts from the press point, so the drag is not lost.
    }
    if (this.panning) {
      const dx = x - this.lastX, dy = y - this.lastY;
      this.lastX = x;
      this.lastY = y;
      return { kind: "pan", dx, dy };
    }
    return { kind: "hover", x, y };
  }

  up(x: number, y: number, button: number): GestureAction {
    if (button !== this.button || !this.active) return NONE;
    const wasClick = this.pending;
    this.cancel();
    return wasClick ? { kind: "click", x, y, button } : NONE;
  }

  cancel(): void {
    this.pending = false;
    this.panning = false;
    this.button = -1;
  }
}

function buttonMask(button: number): number {
  // MouseEvent.buttons: 1 left, 4 middle, 2 right.
  return button === 0 ? 1 : button === 1 ? 4 : button === 2 ? 2 : 0;
}

/** Whether a double-click resets the zoom: only when neither of its clicks
 * landed on a point (the first already selected one). */
export function doubleClickResets(firstHit: boolean, secondHit: boolean): boolean {
  return !firstHit && !secondHit;
}
