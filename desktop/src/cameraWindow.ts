// The camera window of Camera & Alignment (YOFO Studio; Qt Overview-tab parity): the experiment
// window (ROI 1) placed on the whole-sensor image. On the PZ7035 it is the fixed 512x96 Run window
// the backend applies when Experiment opens (#501 P1); elsewhere it is the camera ROI, saved on
// release. The state is kept here, not in App.tsx, so it can be tested.
import {useCallback, useEffect, useRef, useState, type PointerEvent as ReactPointerEvent} from "react";
import {bridge, type CameraGeometry, type InstrumentModeState, type SaveCameraWindowReply} from "./bridge";
import {dragStoredWindow, initialWindow, mirrorOf, pointInDisplayedWindow, snapRunWindow, snapWindow, type Rect} from "./cameraAlignment";

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

/** What the last window save did, for the line under the toolbar (not only the log). */
export interface WindowNotice {level: "info" | "warn" | "error"; text: string}

const baseName = (path: string) => path.split(/[\\/]/).pop() || path;
const describeRect = (r: Rect) => `${r.width}×${r.height} at (${r.x}, ${r.y})`;
const sameRect = (a: Rect | null, b: Rect | null) => !!a && !!b && a.x === b.x && a.y === b.y && a.width === b.width && a.height === b.height;
const asRect = (w: {x: number; y: number; width: number; height: number}): Rect => ({x: w.x, y: w.y, width: w.width, height: w.height});

/** The words for an eGrabber window save (`save_camera_window`): the window as saved (and what it was
 *  snapped from), where it went, and what could not be written. */
export function describeSavedWindow(reply: SaveCameraWindowReply): WindowNotice {
  if (!reply.ok) return {level: "error", text: `Camera window not saved: ${reply.error || "unknown error"}`};
  const saved = asRect(reply.window);
  const parts = [`Camera window ${describeRect(saved)} saved`];
  if (reply.snapped) parts[0] += ` (snapped from ${describeRect(asRect(reply.requested))})`;
  const where: string[] = [];
  if (reply.script.changed) where.push(`script ${baseName(reply.script.path)}`);
  if (reply.profile.config_updated) where.push(`profile ${reply.profile.name}`);
  else if (reply.profile.memory_updated) where.push("the applied profile (this session)");
  if (where.length) parts[0] += ` to ${where.join(" and ")}`;
  let level: WindowNotice["level"] = "info";
  if (reply.script.updated === false && reply.script.path) {
    parts.push(`${baseName(reply.script.path)} has no Width/Height/OffsetX/OffsetY line to change`);
    level = "warn";
  } else if (reply.script.missing.length) {
    parts.push(`${baseName(reply.script.path)} has no ${reply.script.missing.join("/")} line, so that is not saved there`);
    level = "warn";
  }
  if (reply.profile.config_has_window && !reply.profile.config_updated && reply.profile.reason) {
    parts.push(`profile file not changed: ${reply.profile.reason}`);
    level = "warn";
  }
  return {level, text: parts.join("; ") + "."};
}

interface Options {
  instrumentModes: boolean;
  geometryRef: {current: CameraGeometry | null};
  expActive: boolean;
  /** Shared with callers that read the window outside React (the canvas draw loop). */
  windowRef?: {current: Rect | null};
  append: (line: string) => void;
  refreshGeometry: () => Promise<unknown>;
  /** eGrabber: the Experiment script the window is written into ("" = none is selected); resolved when
   *  the save runs, so it follows the operator's current choice. */
  resolveScriptPath?: () => Promise<string>;
  /** Whether the ROI overlay is on (Qt "ROI Overlay: On/Off", #863 A2): while it is off the box is not drawn and cannot be
   *  grabbed. Omitted = always on. */
  overlayRef?: {current: boolean};
}

/** The pause after the last keystroke in a W/H/X/Y box before the window is saved (Qt saves per change). */
export const WINDOW_SAVE_DELAY_MS = 600;

// `window` is the camera window state inside the hook; timers go through the browser window.
const browser: Window = window;

/** The cameras whose window is edited in place and saved at once: an eGrabber camera (its script) and a MindVision camera (its
 *  profile; Qt saves on every W/H change). Aravis keeps its explicit Save. */
const savesOnEdit = (g: CameraGeometry | null | undefined) => g?.camera === "egrabber" || g?.camera === "mindvision";

/** What a refused ROI save says (Qt OverviewTab::saveMindVisionRoi): it stays until a save works. */
export const roiNotSaved = (reason: string) => `ROI was not saved: ${reason || "unknown error"}`;

export function useCameraWindow({instrumentModes, geometryRef, expActive, append, refreshGeometry, windowRef: sharedWindowRef, resolveScriptPath, overlayRef}: Options) {
  const [window, setWindow] = useState<Rect | null>(null);
  // True once the operator placed the window (drag release or Save) or the backend reported the
  // one it applied; a window shown only as the default is not "placed".
  const [placed, setPlaced] = useState(false);
  const ownWindowRef = useRef<Rect | null>(null);
  const windowRef = sharedWindowRef ?? ownWindowRef;
  windowRef.current = window;
  const placedRef = useRef(false);
  placedRef.current = placed;
  // A drag in progress: where the pointer went down (image pixels) and the stored window it started from.
  const dragRef = useRef<{x: number; y: number; start: Rect} | null>(null);
  // eGrabber window saves (#846): serialised, one line of feedback, and a guard so the backend's window
  // (a reloaded or applied profile, Qt reloadCameraWindow) redraws the box without overwriting an edit.
  const [notice, setNotice] = useState<WindowNotice | null>(null);
  const queue = useRef<Promise<void>>(Promise.resolve());
  const generation = useRef(0);          // bumped when a save starts and ends
  const saving = useRef(0);              // saves queued or running
  const pending = useRef(false);         // a W/H/X/Y edit waits for its debounce
  const timer = useRef<number | null>(null);
  const backendWindow = useRef<Rect | null>(null); // the window the backend last told us about
  const lastGood = useRef<Rect | null>(null);        // the last window known to be saved (restored when a save is refused)
  useEffect(() => () => { if (timer.current !== null) browser.clearTimeout(timer.current); }, []);

  /** The window shown when nothing is placed yet. */
  const showDefault = useCallback((geometry: CameraGeometry) => {
    if (geometry.supported && geometry.sensor_width > 0) {
      setWindow((w) => {
        const shown = w ?? initialWindow(geometry);
        lastGood.current ??= shown;
        return shown;
      });
    }
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

  const saveEgrabber = useCallback(async (rect: Rect, geometry: CameraGeometry) => {
    const snapped = snapWindow(rect, geometry);
    windowRef.current = snapped;
    setWindow(snapped);
    let reply: SaveCameraWindowReply;
    try {
      reply = await bridge.saveCameraWindow(resolveScriptPath ? await resolveScriptPath() : "", snapped.x, snapped.y, snapped.width, snapped.height);
    } catch (e) {
      reply = {ok: false, error: String(e), requested: {...snapped}, window: {...snapped}, snapped: false, sensor_width: 0, sensor_height: 0,
        script: {path: "", updated: false, changed: false, missing: []},
        profile: {config_has_window: false, local_profile: false, name: "", memory_updated: false, config_updated: false, reason: ""}};
    }
    const said = describeSavedWindow(reply);
    setNotice(said);
    append(said.text);
    if (reply.ok) {
      const saved = asRect(reply.window);
      backendWindow.current = saved;
      lastGood.current = saved;
      windowRef.current = saved;
      setWindow(saved);
      setPlaced(true);
    }
    await refreshGeometry();
  }, [append, refreshGeometry, resolveScriptPath]);

  /** A refused save puts the old window back and says why, and the line stays until a save works (Qt saveMindVisionRoi). */
  const refused = useCallback((reason: string) => {
    const back = lastGood.current;
    if (back) { windowRef.current = back; setWindow(back); }
    setNotice({level: "error", text: roiNotSaved(reason)});
    append(`Camera ROI not saved: ${reason}`);
  }, [append]);

  const save = useCallback((rect: Rect): Promise<void> => {
    const run = async () => {
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
      try {
        if (geometry.camera === "egrabber") {
          await saveEgrabber(rect, geometry);
          return;
        }
        const snapped = snapWindow(rect, geometry);
        setWindow(snapped);
        const result = await bridge.saveCameraRoi(snapped.x, snapped.y, snapped.width, snapped.height);
        if (result.ok) {
          // A saved window ends the "not saved" line (Qt: the label goes back to its mode text).
          lastGood.current = snapped;
          backendWindow.current = snapped;
          setNotice(null);
          append(result.message);
        } else {
          refused(result.message);
        }
        await refreshGeometry();
      } catch (e) {
        refused(String(e));
      }
    };
    saving.current++;
    generation.current++;
    const next = queue.current.then(run);
    queue.current = next.catch(() => undefined).then(() => { saving.current--; generation.current++; });
    return next;
  }, [append, refreshGeometry, instrumentModes, geometryRef, saveEgrabber, refused]);

  /** The window being edited in a box is saved after a short pause (eGrabber: Qt saves on every change). */
  const flush = useCallback(() => {
    if (timer.current !== null) { browser.clearTimeout(timer.current); timer.current = null; }
    if (!pending.current) return;
    pending.current = false;
    if (windowRef.current && !expActive) void save(windowRef.current);
  }, [save, expActive]);
  const edit = useCallback((key: keyof Rect, value: number) => {
    const current = windowRef.current;
    if (!current || expActive) return;
    const next = {...current, [key]: value};
    windowRef.current = next;
    setWindow(next);
    if (instrumentModes || !savesOnEdit(geometryRef.current)) return;
    pending.current = true;
    if (timer.current !== null) browser.clearTimeout(timer.current);
    timer.current = browser.setTimeout(flush, WINDOW_SAVE_DELAY_MS);
  }, [expActive, instrumentModes, geometryRef, flush]);

  /** Redraw the box from the backend's window when it changed there (a profile applied or reloaded,
   *  Qt reloadCameraWindow), unless the operator is in the middle of an edit or a save is in flight.
   *  `fetchedAt` is the save generation when the geometry was requested: an older reply is ignored. */
  const syncFromGeometry = useCallback((geometry: CameraGeometry, fetchedAt: number) => {
    if (instrumentModes || !geometry.supported || !savesOnEdit(geometry)) return;
    if (fetchedAt !== generation.current || saving.current > 0 || pending.current || dragRef.current) return;
    // An eGrabber geometry says whether it has a window; a MindVision one has its profile's (zeros when it cannot be read).
    if ((geometry.camera === "egrabber" && geometry.window_source === "none") || geometry.roi.width <= 0 || geometry.roi.height <= 0) return;
    const roi = asRect(geometry.roi);
    if (sameRect(backendWindow.current, roi)) return;
    backendWindow.current = roi;
    lastGood.current = roi;
    windowRef.current = roi;
    setWindow(roi);
  }, [instrumentModes]);
  const saveGeneration = useCallback(() => generation.current, []);

  const canvasPoint = (e: ReactPointerEvent<HTMLCanvasElement>) => {
    const canvas = e.currentTarget, box = canvas.getBoundingClientRect();
    return {x: (e.clientX - box.left) * canvas.width / box.width, y: (e.clientY - box.top) * canvas.height / box.height};
  };
  const onPointerDown = (e: ReactPointerEvent<HTMLCanvasElement>) => {
    const geometry = geometryRef.current, rect = windowRef.current;
    if (!geometry?.overview || !rect || expActive) return;
    // Qt grabs the box only while the ROI overlay is on (SimpleImageCanvas::mousePressEvent).
    if (overlayRef && !overlayRef.current) return;
    const p = canvasPoint(e);
    // The box is hit where it is drawn: mirrored on a MindVision camera whose profile flips the image.
    if (!pointInDisplayedWindow(p, rect, {width: geometry.sensor_width, height: geometry.sensor_height}, mirrorOf(geometry))) return;
    dragRef.current = {x: p.x, y: p.y, start: rect};
    e.currentTarget.setPointerCapture?.(e.pointerId);
  };
  const onPointerMove = (e: ReactPointerEvent<HTMLCanvasElement>) => {
    const drag = dragRef.current, geometry = geometryRef.current, rect = windowRef.current;
    if (!drag || !geometry || !rect) return;
    const p = canvasPoint(e);
    // The pointer's movement moves the stored offset the other way on a mirrored axis (Qt mouseMoveEvent).
    const moved = dragStoredWindow(drag.start, {x: p.x - drag.x, y: p.y - drag.y}, mirrorOf(geometry));
    const next = instrumentModes ? snapRunWindow(moved) : snapWindow(moved, geometry);
    windowRef.current = next;
    setWindow(next);
  };
  /** Release saves the window (Qt saves on every move). */
  const onPointerUp = () => {
    if (!dragRef.current) return;
    dragRef.current = null;
    if (windowRef.current) void save(windowRef.current);
  };

  return {
    window, setWindow, placed, windowRef, placedRef, showDefault, restore, save, onPointerDown, onPointerMove, onPointerUp,
    notice, setNotice, edit, flush, syncFromGeometry, saveGeneration,
  };
}
