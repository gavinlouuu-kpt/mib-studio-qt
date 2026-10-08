// The camera window of Camera & Alignment (YOFO Studio; Qt Overview-tab parity): the experiment
// window (ROI 1) placed on the whole-sensor image. On the PZ7035 it is the fixed 512x96 Run window
// the backend applies when Experiment opens (#501 P1); elsewhere it is the camera ROI, saved on
// release. The state is kept here, not in App.tsx, so it can be tested.
import {useCallback, useRef, useState, type PointerEvent as ReactPointerEvent} from "react";
import {bridge, type CameraGeometry, type InstrumentModeState} from "./bridge";
import {initialWindow, snapRunWindow, snapWindow, type Rect} from "./cameraAlignment";

/** The window the backend applied last, or null while no Run switch has succeeded in its process
 *  (run_x/run_y are then the (0, 0) default, not a choice). */
export function restoredRunWindow(mode: InstrumentModeState | undefined): Rect | null {
  return mode?.run_set ? snapRunWindow({x: mode.run_x, y: mode.run_y}) : null;
}

export interface CameraModeError {mode: "align" | "run"; message: string}

/** Why the camera cannot run an experiment on the PZ7035, for the Experiment tab and the Start
 *  button; undefined when it can. The first reason wins: the window must be placed, then the
 *  switch must have worked, then the backend must be in Run. */
export function runModeBlockReason(s: {windowPlaced: boolean; modeError: CameraModeError | null; runMode: boolean}): string | undefined {
  if (!s.windowPlaced) return "Place the 512×96 run window in Camera & Alignment first";
  if (s.modeError?.mode === "run") return `The camera did not switch to Run: ${s.modeError.message}`;
  if (!s.runMode) return "The camera is not in Run mode yet";
  return undefined;
}

interface Options {
  instrumentModes: boolean;
  geometryRef: {current: CameraGeometry | null};
  expActive: boolean;
  /** Shared with callers that read the window outside React (the canvas draw loop). */
  windowRef?: {current: Rect | null};
  append: (line: string) => void;
  refreshGeometry: () => Promise<unknown>;
}

export function useCameraWindow({instrumentModes, geometryRef, expActive, append, refreshGeometry, windowRef: sharedWindowRef}: Options) {
  const [window, setWindow] = useState<Rect | null>(null);
  // True once the operator placed the window (drag release or Save) or the backend reported the
  // one it applied; a window shown only as the default is not "placed".
  const [placed, setPlaced] = useState(false);
  const ownWindowRef = useRef<Rect | null>(null);
  const windowRef = sharedWindowRef ?? ownWindowRef;
  windowRef.current = window;
  const placedRef = useRef(false);
  placedRef.current = placed;
  const dragRef = useRef<{dx: number; dy: number} | null>(null);

  /** The window shown when nothing is placed yet. */
  const showDefault = useCallback((geometry: CameraGeometry) => {
    if (geometry.supported && geometry.sensor_width > 0) setWindow((w) => w ?? initialWindow(geometry));
  }, []);

  /** Take the backend's window (after a page reload); an operator's own placement wins. */
  const restore = useCallback((mode: InstrumentModeState | undefined) => {
    const restored = restoredRunWindow(mode);
    if (!restored || placedRef.current) return;
    windowRef.current = restored;
    placedRef.current = true;
    setWindow(restored);
    setPlaced(true);
  }, []);

  const save = useCallback(async (rect: Rect) => {
    if (instrumentModes) {
      // The Run window is applied (and saved) by the switch to Run when Experiment opens.
      const snapped = snapRunWindow(rect);
      setWindow(snapped);
      setPlaced(true);
      append(`Run window (${snapped.x}, ${snapped.y}) 512×96: applied when Experiment opens`);
      return;
    }
    const geometry = geometryRef.current;
    if (!geometry?.supported) return;
    const snapped = snapWindow(rect, geometry);
    setWindow(snapped);
    const result = await bridge.saveCameraRoi(snapped.x, snapped.y, snapped.width, snapped.height);
    append(result.ok ? result.message : `Camera ROI not saved: ${result.message}`);
    await refreshGeometry();
  }, [append, refreshGeometry, instrumentModes, geometryRef]);

  const canvasPoint = (e: ReactPointerEvent<HTMLCanvasElement>) => {
    const canvas = e.currentTarget, box = canvas.getBoundingClientRect();
    return {x: (e.clientX - box.left) * canvas.width / box.width, y: (e.clientY - box.top) * canvas.height / box.height};
  };
  const onPointerDown = (e: ReactPointerEvent<HTMLCanvasElement>) => {
    const geometry = geometryRef.current, rect = windowRef.current;
    if (!geometry?.overview || !rect || expActive) return;
    const p = canvasPoint(e);
    if (p.x < rect.x || p.y < rect.y || p.x > rect.x + rect.width || p.y > rect.y + rect.height) return;
    dragRef.current = {dx: p.x - rect.x, dy: p.y - rect.y};
    e.currentTarget.setPointerCapture?.(e.pointerId);
  };
  const onPointerMove = (e: ReactPointerEvent<HTMLCanvasElement>) => {
    const drag = dragRef.current, geometry = geometryRef.current, rect = windowRef.current;
    if (!drag || !geometry || !rect) return;
    const p = canvasPoint(e);
    const next = instrumentModes
      ? snapRunWindow({x: p.x - drag.dx, y: p.y - drag.dy})
      : snapWindow({...rect, x: p.x - drag.dx, y: p.y - drag.dy}, geometry);
    windowRef.current = next;
    setWindow(next);
  };
  /** Release saves the window (Qt saves on every move). */
  const onPointerUp = () => {
    if (!dragRef.current) return;
    dragRef.current = null;
    if (windowRef.current) void save(windowRef.current);
  };

  return {window, setWindow, placed, windowRef, placedRef, showDefault, restore, save, onPointerDown, onPointerMove, onPointerUp};
}
