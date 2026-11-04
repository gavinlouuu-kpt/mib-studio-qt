// Connect and Overview (epic #863): camera discovery and selection, the mock camera, camera
// start/stop, the hardware preflight checklist, the camera window and the Align/Run mode switch,
// and the Overview quality gates. Moved from App.tsx without change (#869 wave 0);
// studio/useStudioController.ts calls these hooks in App.tsx's original effect order.
import { useCallback, useEffect, useRef, useState } from "react";
import { bridge, type CameraDiscovery, type CameraGeometry, type CameraSelection, type RecordingCapacity } from "../../bridge";
import { PUMP_IDS } from "../../bridgeContract";
import { snapRunWindow, type Rect } from "../../cameraAlignment";
import { runModeBlockReason, useCameraWindow, type CameraModeError } from "../../cameraWindow";
import { useViewerOnly } from "../../components/ViewerGate";
import { invoke } from "../../transport";
import { derivePreflight, type PreflightInput } from "../../preflight";
import { deriveQualityGates, type ImageQualityInput, type QualityInput } from "../../quality";
import type { RunFpsStatus } from "../../runFps";
import type { CameraSetupModel, ConfigDocumentsModel, ConfigState, CoresModel } from "../config/useConfig";
import type { HardwareState } from "../hardware/useHardware";
import type { MonitoringState } from "../monitoring/useMonitoring";
import type { LiveFramesModel, PreviewState } from "../preview/usePreview";
import type { ReviewState } from "../review/useReview";
import type { ShellState, StatePollingModel } from "../shell/useShell";
import {
  isEgrabber, NO_SOURCES, resolveExperimentScript, runEgrabberStage, runOverviewAutoStart,
  type ScriptSources, type StageIo, type StageName, type StageNotice,
} from "./cameraStage";
import { roiSummary } from "./overviewInfo";
import { ensureOverviewDefault } from "./overviewScript";
import type { OverviewScriptModel } from "./useOverviewScript";
import { connectedCameraKey, overlaySwitchesOn } from "./roiOverlay";

export function useConnectState() {
  const [running, setRunning] = useState(false);
  const [camStatus, setCamStatus] = useState("unconfigured");
  const [connectTab, setConnectTab] = useState<"cameras" | "mindvision" | "framegrabbers">("cameras");

  // Camera discovery/selection (bridge schema v7, BE-2). The selection
  // snapshot from the backend is authoritative — no local mirror of it.
  const [discovery, setDiscovery] = useState<CameraDiscovery | null>(null);
  const [camSelection, setCamSelection] = useState<CameraSelection | null>(null);
  const [pickedDevice, setPickedDevice] = useState<string | null>(null);

  // Mock camera configuration modal.
  const [showMockConfig, setShowMockConfig] = useState(false);
  const [frameDir, setFrameDir] = useState("");
  const [intervalMs, setIntervalMs] = useState("33");
  const [loopFiles, setLoopFiles] = useState(true);
  const liveCanvasRef = useRef<HTMLCanvasElement>(null);
  // ---- Camera & Alignment (ABI 20): the whole sensor and the experiment window on it ----
  // PZ7035 Align (#501): the live frame's focus and brightness in the Run window, with the best
  // focus seen since the window last moved. draw() measures every displayed frame (about 50 k
  // pixels) and publishes at most every 200 ms.
  const [alignImage, setAlignImage] = useState<ImageQualityInput | null>(null);
  const alignBestRef = useRef<{ best: number | null; box: Rect | null }>({ best: null, box: null });
  const alignImageMs = useRef(-Infinity);
  const [cameraGeometry, setCameraGeometry] = useState<CameraGeometry | null>(null);
  const cameraGeometryRef = useRef<CameraGeometry | null>(null);
  cameraGeometryRef.current = cameraGeometry;
  const cameraWindowRef = useRef<Rect | null>(null);
  const refreshCameraGeometryRef = useRef<() => Promise<unknown>>(async () => undefined);
  // Why the last Align/Run switch was refused (shown in the tab, not only logged); null when it worked.
  const [modeError, setModeError] = useState<CameraModeError | null>(null);
  const [modeRetry, setModeRetry] = useState(0);
  // The Run frame rate and exposure (ABI 38): polled on a PZ7035; set by the PZ7035 camera document (Config → Camera document), which re-enters Run when the instrument is in it.
  const [runFpsStatus, setRunFpsStatus] = useState<RunFpsStatus | null>(null);
  // What entering the Overview / Experiment stage did to the camera (#846), why the geometry could not be
  // read, the Recording capacity of the next run (#863) and where the camera scripts may come from.
  const [stageNotice, setStageNotice] = useState<StageNotice | null>(null);
  const [geometryError, setGeometryError] = useState("");
  const [capacity, setCapacity] = useState<RecordingCapacity | null>(null);
  const scriptSourcesRef = useRef<ScriptSources>(NO_SOURCES);
  // The Overview's "ROI Overlay: On/Off" (Qt roiOverlayBtn, #863 CONNECT-047): off until a camera is connected. The ref is what the
  // preview's draw() and the drag read (effective visibility, set in useCameraWindowing); PZ7035 always shows its Run window.
  const [roiOverlay, setRoiOverlay] = useState(false);
  const roiOverlayRef = useRef(false);
  return {
    running, setRunning, camStatus, setCamStatus, connectTab, setConnectTab,
    discovery, setDiscovery, camSelection, setCamSelection, pickedDevice, setPickedDevice,
    showMockConfig, setShowMockConfig, frameDir, setFrameDir, intervalMs, setIntervalMs, loopFiles, setLoopFiles,
    liveCanvasRef, alignImage, setAlignImage, alignBestRef, alignImageMs,
    cameraGeometry, setCameraGeometry, cameraGeometryRef, cameraWindowRef, refreshCameraGeometryRef,
    modeError, setModeError, modeRetry, setModeRetry, runFpsStatus, setRunFpsStatus,
    stageNotice, setStageNotice, geometryError, setGeometryError, capacity, setCapacity, scriptSourcesRef,
    roiOverlay, setRoiOverlay, roiOverlayRef,
  };
}
export type ConnectState = ReturnType<typeof useConnectState>;

type AlignPeakDeps = Pick<ShellState, "instrumentModeName"> & Pick<ConnectState, "alignBestRef" | "setAlignImage">;

export function useAlignPeakReset({ instrumentModeName, alignBestRef, setAlignImage }: AlignPeakDeps) {
  // A mode switch (or leaving Align) starts a fresh peak and clears the numbers.
  useEffect(() => {
    alignBestRef.current = { best: null, box: null };
    setAlignImage(null);
  }, [instrumentModeName]);
}

type ConnectionDeps = Pick<ShellState, "ready" | "tab" | "setTab" | "append">
  & Pick<ConnectState, "setDiscovery" | "setCamSelection" | "setShowMockConfig" | "setCamStatus" | "frameDir" | "intervalMs"
    | "loopFiles" | "pickedDevice" | "discovery" | "setRunning">
  & Pick<ConfigState, "setCoreStatus"> & Pick<HardwareState, "setAfStatus" | "setSamplePump" | "setSheathPump" | "setTrigStatus">
  & Pick<ReviewState, "setReviewing"> & Pick<PreviewState, "recording" | "setRecording">
  & Pick<StatePollingModel, "tick"> & Pick<LiveFramesModel, "stopLoop">;

// Discovery and selection, the Preflight polling, the mock camera and Start/Stop Camera.
export function useCameraConnection({
  ready, tab, setTab, append, setDiscovery, setCamSelection, setShowMockConfig, setCamStatus, frameDir, intervalMs,
  loopFiles, pickedDevice, discovery, setRunning, setCoreStatus, setAfStatus, setSamplePump, setSheathPump, setTrigStatus,
  setReviewing, recording, setRecording, tick, stopLoop,
}: ConnectionDeps) {
  // ---- Camera discovery/selection (BE-2) + camera actions ----

  const refreshCameraState = useCallback(async () => {
    try {
      setDiscovery(await bridge.discoverCameras());
      setCamSelection(await bridge.fetchCameraSelection());
    } catch (e) {
      append(`discovery error: ${e}`);
    }
  }, [append]);

  useEffect(() => {
    if (ready) void refreshCameraState();
  }, [ready, refreshCameraState]);

  // UX-3: poll every subsystem the preflight checklist needs. Runs off the
  // capture loop so preflight works before the camera is started.
  const refreshPreflight = useCallback(async () => {
    try {
      const [sel, core, af, sample, sheath, trig] = await Promise.all([
        bridge.fetchCameraSelection(),
        bridge.fetchProcessingCoreStatus(),
        bridge.fetchAutofocusStatus(),
        bridge.fetchPumpStatus(PUMP_IDS.Sample),
        bridge.fetchPumpStatus(PUMP_IDS.Sheath),
        bridge.fetchTriggerStatus(),
      ]);
      setCamSelection(sel);
      setCoreStatus(core);
      setAfStatus(af);
      setSamplePump(sample);
      setSheathPump(sheath);
      setTrigStatus(trig);
    } catch {
      /* backend gone — the capture loop / next poll surfaces it */
    }
  }, []);

  useEffect(() => {
    if (!ready || tab !== "connect") return;
    void refreshPreflight();
    const id = window.setInterval(() => void refreshPreflight(), 1500);
    return () => window.clearInterval(id);
  }, [ready, tab, refreshPreflight]);

  const onConfigureMock = useCallback(async () => {
    try {
      const cfg = await bridge.configureMock(frameDir, Number(intervalMs) || 33, loopFiles);
      if (!cfg.ok) return append(`configure failed: ${cfg.message}`);
      setShowMockConfig(false);
      setCamStatus("configured (mock)");
      append(`mock camera configured (${frameDir})`);
      await refreshCameraState();
    } catch (e) {
      append(`configure error: ${e}`);
    }
  }, [frameDir, intervalMs, loopFiles, append, refreshCameraState]);

  const onConnectPicked = useCallback(async () => {
    if (!pickedDevice || !discovery) return;
    const cam = discovery.cameras.find(
      (c) => `${c.camera_type}:${c.interface_index}:${c.device_index}:${c.camera_index}` === pickedDevice,
    );
    if (!cam) return;
    try {
      if (cam.camera_type === 2) {
        // Mock source: configuration happens through the modal.
        setShowMockConfig(true);
        return;
      }
      const res =
        cam.camera_type === 1
          ? await bridge.selectMindVisionCamera(cam.camera_index, cam.label, "")
          : await bridge.selectHardwareCamera(cam.interface_index, cam.device_index, cam.label);
      append(res.ok ? `selected ${cam.label}` : `select failed: ${res.message}`);
      await refreshCameraState();
    } catch (e) {
      append(`select error: ${e}`);
    }
  }, [pickedDevice, discovery, append, refreshCameraState]);

  const onStartCamera = useCallback(async () => {
    try {
      setReviewing(false);
      const res = await bridge.startCapture();
      if (!res.ok) return append(`start failed: ${res.message}`);
      setRunning(true);
      append("capture start requested; waiting for camera-ready confirmation");

      // Parity with Qt: a successful start lands the operator on Overview.
      setTab((t) => (t === "connect" ? "overview" : t));
    } catch (e) {
      append(`start error: ${e}`);
    }
  }, [tick, append, stopLoop]);

  const onStopCamera = useCallback(async () => {
    try {
      if (recording) {
        const stopped = await bridge.stopRecording();
        if (!stopped.ok) return append(`stop recording failed: ${stopped.message}`);
        setRecording(false);
      }
      const stopped = await bridge.stopCapture();
      if (!stopped.ok) return append(`stop capture failed: ${stopped.message}`);
      setRunning(false);
      append("capture stopped");
    } catch (e) {
      append(`stop error: ${e}`);
    }
  }, [recording, stopLoop, append]);
  return { refreshCameraState, refreshPreflight, onConfigureMock, onConnectPicked, onStartCamera, onStopCamera };
}
export type CameraConnectionModel = ReturnType<typeof useCameraConnection>;

type WindowingDeps = Pick<ShellState, "ready" | "tab" | "hostProcessing" | "instrumentModes" | "append">
  & Pick<ConnectState, "cameraGeometryRef" | "cameraWindowRef" | "refreshCameraGeometryRef" | "setCameraGeometry" | "setRunFpsStatus"
    | "scriptSourcesRef" | "setGeometryError" | "setCapacity" | "camSelection" | "roiOverlay" | "setRoiOverlay" | "roiOverlayRef">
  & Pick<PreviewState, "framePulls"> & Pick<MonitoringState, "expActive">;

/** Whether a script file can be read: an existing .js file the camera document command opens. */
export async function scriptExists(path: string): Promise<boolean> {
  try {
    await invoke("camera_document", { action: "read", path, kind: "js", baseline: "", text: "" });
    return true;
  } catch {
    return false;
  }
}

// The camera window (ROI 1 on the full sensor), the Recording capacity of the next run and the PZ7035 Run frame rate.
export function useCameraWindowing({
  ready, tab, hostProcessing, instrumentModes, append, cameraGeometryRef, cameraWindowRef, refreshCameraGeometryRef, setCameraGeometry,
  setRunFpsStatus, expActive, scriptSourcesRef, setGeometryError, setCapacity, camSelection, roiOverlay, setRoiOverlay, roiOverlayRef, framePulls,
}: WindowingDeps) {
  // The box is drawn and grabbed only while the overlay is on; the PZ7035 has no toggle and always shows its Run window.
  roiOverlayRef.current = instrumentModes || roiOverlay;
  const cameraWin = useCameraWindow({
    instrumentModes, geometryRef: cameraGeometryRef, expActive, append, windowRef: cameraWindowRef, overlayRef: roiOverlayRef,
    refreshGeometry: () => refreshCameraGeometryRef.current(),
    // An eGrabber window is written into the Experiment script (#846): the operator's current choice.
    resolveScriptPath: async () => (await resolveExperimentScript(scriptSourcesRef.current, scriptExists))?.path ?? "",
  });
  const cameraWindow = cameraWin.window, setCameraWindow = cameraWin.setWindow;
  const lastGeometryError = useRef("");
  const refreshCameraGeometry = useCallback(async (): Promise<CameraGeometry | null> => {
    const fetchedAt = cameraWin.saveGeneration();
    try {
      const geometry = await bridge.fetchCameraGeometry();
      setCameraGeometry(geometry);
      setGeometryError("");
      lastGeometryError.current = "";
      cameraWin.syncFromGeometry(geometry, fetchedAt);
      cameraWin.showDefault(geometry);
      return geometry;
    } catch (e) {
      // Shown in the Overview as well as logged (once per change: the poll retries every 2 s).
      const text = `Camera geometry unavailable: ${e instanceof Error ? e.message : String(e)}`;
      setGeometryError(text);
      if (lastGeometryError.current !== text) { lastGeometryError.current = text; append(text); }
      return null;
    }
  }, [cameraWin.showDefault, cameraWin.syncFromGeometry, cameraWin.saveGeneration, append]);
  refreshCameraGeometryRef.current = refreshCameraGeometry;

  // Recording capacity (#833, Qt MainWindow::refreshRecordingCapacity): once a second on the Overview, with the
  // box as the planned window; frozen while an experiment runs; the last view is kept when the backend is busy.
  useEffect(() => {
    if (!ready || tab !== "overview" || !hostProcessing || instrumentModes || expActive) return;
    let live = true;
    const poll = async () => {
      try {
        const box = cameraGeometryRef.current?.supported ? cameraWindowRef.current : null;
        const view = await bridge.fetchRecordingCapacity(box ?? undefined);
        if (live && !view.busy) setCapacity(view);
      } catch {
        /* keep the last view; the next poll tries again */
      }
    };
    void poll();
    const id = window.setInterval(() => void poll(), 1000);
    return () => { live = false; window.clearInterval(id); };
  }, [ready, tab, hostProcessing, instrumentModes, expActive]);

  const refreshRunFps = useCallback(async () => {
    if (!instrumentModes) return;
    try { setRunFpsStatus(await bridge.fetchRunFps()); } catch { /* the next poll tries again */ }
  }, [instrumentModes]);
  useEffect(() => {
    if (!ready || !instrumentModes) return;
    void refreshRunFps();
    const id = window.setInterval(() => void refreshRunFps(), 3000);
    return () => window.clearInterval(id);
  }, [ready, instrumentModes, refreshRunFps]);
  // CONNECT-047: connecting a camera switches the overlay on, once per camera (Qt: ConnectTab::connected).
  const connectedKey = connectedCameraKey(camSelection);
  const lastConnectedKey = useRef("");
  useEffect(() => {
    const previous = lastConnectedKey.current;
    lastConnectedKey.current = connectedKey;
    if (overlaySwitchesOn(previous, connectedKey)) setRoiOverlay(true);
  }, [connectedKey]);
  const toggleRoiOverlay = useCallback(() => {
    setRoiOverlay((on) => !on);
    // The box is painted with the frame: pull the last one so a stopped camera shows the change at once.
    framePulls.current.request("live", bridge.fetchFrame, false);
  }, []);
  return { cameraWin, cameraWindow, setCameraWindow, refreshCameraGeometry, toggleRoiOverlay };
}
export type CameraWindowingModel = ReturnType<typeof useCameraWindowing>;

type ModeSwitchDeps = Pick<ShellState, "ready" | "tab" | "tabRef" | "append" | "instrumentModes" | "instrumentRef" | "instrumentMode"
    | "instrument" | "runMode">
  & Pick<ConnectState, "cameraWindowRef" | "setModeError" | "modeRetry" | "modeError" | "setRunning" | "scriptSourcesRef" | "setStageNotice">
  & Pick<MonitoringState, "expActive"> & Pick<PreviewState, "recording"> & Pick<ReviewState, "setReviewing"> & CameraWindowingModel;

// Qt parity: the tab picks the camera mode (Align / full sensor on Overview, Run / the window on Experiment).
export function useCameraModeSwitch({
  ready, tab, tabRef, append, instrumentModes, instrumentRef, instrumentMode, instrument, runMode, cameraWindowRef, setModeError,
  modeRetry, modeError, setRunning, expActive, recording, setReviewing, cameraWin, refreshCameraGeometry, scriptSourcesRef, setStageNotice,
}: ModeSwitchDeps) {
  // A browser that only watches (#763) never changes the camera: its stage tabs are navigation, and the
  // controlling client's own change shows through the polled geometry.
  const viewerOnly = useViewerOnly();
  // After a page reload the backend still holds the Run window it applied: take it back.
  useEffect(() => { cameraWin.restore(instrumentMode); }, [instrumentMode?.run_set, instrumentMode?.run_x, instrumentMode?.run_y, cameraWin.restore]);

  // Qt parity (MainWindow tab change): Camera & Alignment shows the whole sensor, Experiment the
  // saved window, other tabs leave the camera as it is; never during a run. A running capture
  // restarts in the new mode (backend).
  // An eGrabber camera has no preset: entering a stage applies its script with that stage's role (#846),
  // once per entry, and Overview starts a stopped, configured camera (cameraStage.ts).
  const entered = useRef<StageName | null>(null);
  const stageQueue = useRef<Promise<void>>(Promise.resolve());
  const live = useRef({ expActive, recording });
  live.current = { expActive, recording };
  useEffect(() => {
    if (!ready || (tab !== "overview" && tab !== "experiment")) { entered.current = null; return; }
    let cancelled = false;
    void (async () => {
      const geometry = await refreshCameraGeometry();
      if (cancelled) return;
      const firstEntry = entered.current !== tab;
      entered.current = tab;
      if (expActive) return;
      if (instrumentModes) {
        // PZ7035 (#501 P1): the backend owns the switch (LED, cell path, producer timing).
        const want = tab === "overview" ? "align" : "run";
        const current = instrumentRef.current?.mode;
        const win = cameraWindowRef.current ? snapRunWindow(cameraWindowRef.current) : null;
        // No placed window: stay as we are; the Experiment tab says so and offers the way there.
        if (want === "run" && (!win || !cameraWin.placedRef.current)) return;
        if (current?.name === want && (want === "align" || (current.run_x === win!.x && current.run_y === win!.y))) { setModeError(null); return; }
        // A stopped Run holds the frames the operator stopped to review: a tab change must not silently discard them (the backend refuses the
        // switch as well). Resume Run ends it, and this effect then switches to the tab's mode.
        if (instrumentRef.current?.ring?.run_frozen) {
          append("Run is stopped to review the buffered frames: Resume Run before changing the camera mode");
          return;
        }
        append(want === "align" ? "switching to Align (full sensor)…" : `switching to Run at (${win!.x}, ${win!.y})…`);
        const result = await bridge.setInstrumentMode(want, win?.x ?? 0, win?.y ?? 0);
        append(result.ok ? result.message : `Camera mode: ${result.message}`);
        if (cancelled) return;
        setModeError(result.ok ? null : {mode: want, message: result.message});
        setRunning(result.ok && want === "align");
        await refreshCameraGeometry();
        return;
      }
      if (viewerOnly) return;
      if (isEgrabber(geometry)) {
        if (!firstEntry) return; // the scripts are applied once per entry, not on every effect re-run
        setStageNotice(null);
        const stage: StageName = tab;
        const io: StageIo = {
          fetchSelection: () => bridge.fetchCameraSelection(),
          stopCapture: () => bridge.stopCapture(),
          startCapture: async () => { setReviewing(false); return bridge.startCapture(); },
          applyScript: (path, overview) => bridge.applyCameraScriptWithRole(path, overview),
          refreshGeometry: () => refreshCameraGeometry(),
          scriptExists,
          createOverviewDefault: ensureOverviewDefault,
          setRunning,
          log: append,
        };
        // Serialised, and dropped when the operator has moved on before its turn: a script apply is not interrupted.
        stageQueue.current = stageQueue.current.then(async () => {
          if (tabRef.current !== stage) return;
          const notice = await runEgrabberStage({
            stage, expActive: live.current.expActive, recording: live.current.recording, sources: scriptSourcesRef.current, geometry,
          }, io);
          setStageNotice(notice);
        }).catch(() => undefined);
        return;
      }
      if (geometry?.supported) {
        const overview = tab === "overview";
        if (geometry.overview !== overview) {
          const result = await bridge.setCameraOverview(overview);
          append(result.ok ? result.message : `Camera mode: ${result.message}`);
          if (!cancelled) await refreshCameraGeometry();
        }
      }
      // Overview starts a stopped, configured camera, once per entry (not an illuminated MindVision profile).
      if (tab === "overview" && firstEntry && !cancelled) {
        const fresh = geometry?.supported ? await refreshCameraGeometry() : geometry;
        const notice = await runOverviewAutoStart({ expActive: live.current.expActive, geometry: fresh, instrumentModes }, {
          fetchSelection: () => bridge.fetchCameraSelection(),
          startCapture: async () => { setReviewing(false); return bridge.startCapture(); },
          setRunning,
          log: append,
        });
        setStageNotice(notice);
      }
    })();
    return () => { cancelled = true; };
  }, [tab, ready, expActive, refreshCameraGeometry, append, instrumentModes, cameraWin.placed, modeRetry, instrumentMode?.idle, instrument?.ring?.run_frozen, viewerOnly]);

  // The camera's read-back (applied window, sensor and delivered rate) follows its restart.
  useEffect(() => {
    if (!ready || tab !== "overview") return;
    const id = window.setInterval(() => void refreshCameraGeometry(), 2000);
    return () => window.clearInterval(id);
  }, [ready, tab, refreshCameraGeometry]);

  const saveCameraWindow = cameraWin.save;
  const onWindowPointerDown = cameraWin.onPointerDown, onWindowPointerMove = cameraWin.onPointerMove, onWindowPointerUp = cameraWin.onPointerUp;

  // PZ7035: the PL runs the experiment, so what matters is the backend's Run mode, not a live
  // camera stream (Run stops the producer); a missing window or a refused switch says why.
  const runModeBlock = instrumentModes ? runModeBlockReason({windowPlaced: cameraWin.placed, modeError, runMode}) : undefined;
  return { saveCameraWindow, onWindowPointerDown, onWindowPointerMove, onWindowPointerUp, runModeBlock };
}

// Where the camera scripts may come from, read from the config area's state each render (no hook): the
// stage change and the window save read the operator's current choice through scriptSourcesRef, which the
// connect hooks above own but the config hooks, called later, fill.
export function syncScriptSources({ scriptSourcesRef, cameraScript, scriptDocument, profiles, overviewScript }:
  Pick<ConnectState, "scriptSourcesRef"> & Pick<CameraSetupModel, "cameraScript" | "scriptDocument"> & Pick<ConfigDocumentsModel, "profiles">
  & { overviewScript: Pick<OverviewScriptModel, "path" | "defaultPath"> }) {
  scriptSourcesRef.current = {
    ...scriptSourcesRef.current,
    cameraScriptPath: cameraScript.path,
    cameraDocumentPath: scriptDocument.doc?.path ?? "",
    profileConfigPath: profiles.activeProfile?.path ?? "",
    // The Overview editor's resolved path (Qt currentJsPath), so the stage change applies the file the operator edits.
    overviewScriptPath: overviewScript.path,
    overviewDefaultPath: overviewScript.defaultPath,
  };
}

type ConnectModelDeps = Pick<ShellState, "ready" | "caps" | "instrument" | "pz7035" | "instrumentModes">
  & Pick<ConnectState, "camSelection" | "running" | "alignImage" | "cameraGeometry"> & Pick<CameraWindowingModel, "cameraWindow">
  & Pick<ConfigState, "coreStatus" | "stats">
  & Pick<HardwareState, "afStatus" | "samplePump" | "sheathPump" | "trigStatus">
  & Pick<PreviewState, "backgroundSet" | "roiFields" | "lastMeta"> & CoresModel & Pick<CameraSetupModel, "cameraScript">;

// Render-time values: the Start Camera gate, the UX-3 preflight checklist and the UX-4 quality gates.
export function connectModel({
  ready, caps, instrument, pz7035, instrumentModes, camSelection, running, alignImage, coreStatus, stats, afStatus,
  samplePump, sheathPump, trigStatus, backgroundSet, roiFields, lastMeta, cores, cameraScript, cameraGeometry, cameraWindow,
}: ConnectModelDeps) {
  const cameraConfigured = camSelection?.configured ?? false;
  const startCameraReason = cores.appUpdateBusy ? "Application update is pending" : cameraScript.busy ? "Camera setup is in progress" : !ready
    ? "Backend is not initialized"
    : !cameraConfigured
      ? "No camera configured — select a device in the Connect tab"
      : running
        ? "Camera is already running"
        : undefined;

  // ---- UX-3 profile-aware hardware preflight (issue #307) ----
  const preflightInput: PreflightInput = {
    backendReady: ready,
    cameraConfigured,
    cameraRunning: running,
    cameraIdentity: camSelection?.label || (camSelection?.mode === 1 ? "Mock camera" : ""),
    cameraExpected: "", // profile-declared expected identity — UX-2 (#306) follow-up
    coreValid: coreStatus?.valid ?? false,
    corePinSatisfied: coreStatus?.pin_satisfied ?? false,
    coreVersion: coreStatus?.active_version ?? "",
    requiredCoreVersion: coreStatus?.required_version ?? "",
    autofocus: {
      valid: afStatus?.valid ?? false,
      connected: afStatus?.connected ?? false,
      identity: afStatus?.connected ? (afStatus.endpoint_id || `${afStatus.backend_name || "Controller"} COM${afStatus.com_port}`) : "",
    },
    samplePump: {
      valid: samplePump?.valid ?? false,
      connected: samplePump?.connected ?? false,
      identity: samplePump?.connected ? (samplePump.port_name || `COM${samplePump.com_port}`) : "",
    },
    sheathPump: {
      valid: sheathPump?.valid ?? false,
      connected: sheathPump?.connected ?? false,
      identity: sheathPump?.connected ? (sheathPump.port_name || `COM${sheathPump.com_port}`) : "",
    },
    trigger: { valid: trigStatus?.valid ?? false, cameraAttached: trigStatus?.camera_attached ?? false },
    // The PZ7035 reports its recording target (#501); the desktop's check stays informational.
    storageKnown: !!instrument?.storage,
    storageWritable: instrument?.storage?.writable ?? false,
    storageFreeOk: (instrument?.storage?.free_bytes ?? 0) >= 1e9,
    storagePath: instrument?.storage?.path ?? "",
    storageWarning: instrument?.storage?.warning ?? "",
    capabilities: caps,
    instrument,
  };
  const preflight = derivePreflight(preflightInput);

  // ---- UX-4 Camera & Alignment quality gates (issue #308) ----
  // Focus staleness threshold: the autofocus config's ring_ratio_stale_ms is
  // not polled into the shell yet, so use a conservative default until it is.
  const FOCUS_STALE_MS = 1000;
  const qualityInput: QualityInput = {
    cameraRunning: running,
    focusConnected: afStatus?.connected ?? false,
    focusMetric: afStatus?.median_ring_ratio ?? 0,
    focusUpdatedUs: afStatus?.last_ring_ratio_update_us ?? 0,
    focusAgeMs: afStatus ? afStatus.ring_ratio_age_us / 1000 : 0,
    focusStaleMs: FOCUS_STALE_MS,
    backgroundSet,
    roiW: Number(roiFields.w) || 0,
    roiH: Number(roiFields.h) || 0,
    frameW: lastMeta?.width ?? 0,
    frameH: lastMeta?.height ?? 0,
    pixelToMicron: stats?.pixel_to_micron ?? NaN,
    pz7035,
    // Only while Align shows the full sensor; Run has no live camera frame to measure.
    image: pz7035 && instrumentModes
      ? (instrument?.mode?.name === "align" && alignImage ? alignImage : { metrics: null, best: null })
      : undefined,
  };
  const quality = deriveQualityGates(qualityInput);
  // The "ROI: W x H @ (x, y)" label of the Experiment header follows the Overview rectangle (Qt roiLabel_, CONNECT-064).
  const roi = roiSummary({ geometry: cameraGeometry, window: cameraWindow, instrumentModes, processing: roiFields });
  return { cameraConfigured, startCameraReason, preflight, qualityInput, quality, roiSummaryText: roi.text, roiSummaryTitle: roi.title };
}
export type ConnectModel = ReturnType<typeof connectModel>;

