// Connect and Overview (epic #863): camera discovery and selection, the mock camera, camera
// start/stop, the hardware preflight checklist, the camera window and the Align/Run mode switch,
// and the Overview quality gates. Moved from App.tsx without change (#869 wave 0);
// studio/useStudioController.ts calls these hooks in App.tsx's original effect order.
import { useCallback, useEffect, useRef, useState } from "react";
import { bridge, type CameraDiscovery, type CameraGeometry, type CameraSelection } from "../../bridge";
import { PUMP_IDS } from "../../bridgeContract";
import { snapRunWindow, type Rect } from "../../cameraAlignment";
import { runModeBlockReason, useCameraWindow, type CameraModeError } from "../../cameraWindow";
import { derivePreflight, type PreflightInput } from "../../preflight";
import { deriveQualityGates, type ImageQualityInput, type QualityInput } from "../../quality";
import type { RunFpsStatus } from "../../runFps";
import type { CameraSetupModel, ConfigState, CoresModel } from "../config/useConfig";
import type { HardwareState } from "../hardware/useHardware";
import type { MonitoringState } from "../monitoring/useMonitoring";
import type { LiveFramesModel, PreviewState } from "../preview/usePreview";
import type { ReviewState } from "../review/useReview";
import type { ShellState, StatePollingModel } from "../shell/useShell";

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
  return {
    running, setRunning, camStatus, setCamStatus, connectTab, setConnectTab,
    discovery, setDiscovery, camSelection, setCamSelection, pickedDevice, setPickedDevice,
    showMockConfig, setShowMockConfig, frameDir, setFrameDir, intervalMs, setIntervalMs, loopFiles, setLoopFiles,
    liveCanvasRef, alignImage, setAlignImage, alignBestRef, alignImageMs,
    cameraGeometry, setCameraGeometry, cameraGeometryRef, cameraWindowRef, refreshCameraGeometryRef,
    modeError, setModeError, modeRetry, setModeRetry, runFpsStatus, setRunFpsStatus,
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

type WindowingDeps = Pick<ShellState, "ready" | "instrumentModes" | "append">
  & Pick<ConnectState, "cameraGeometryRef" | "cameraWindowRef" | "refreshCameraGeometryRef" | "setCameraGeometry" | "setRunFpsStatus">
  & Pick<MonitoringState, "expActive">;

// The camera window (ROI 1 on the full sensor) and the PZ7035 Run frame rate.
export function useCameraWindowing({
  ready, instrumentModes, append, cameraGeometryRef, cameraWindowRef, refreshCameraGeometryRef, setCameraGeometry,
  setRunFpsStatus, expActive,
}: WindowingDeps) {
  const cameraWin = useCameraWindow({
    instrumentModes, geometryRef: cameraGeometryRef, expActive, append, windowRef: cameraWindowRef,
    refreshGeometry: () => refreshCameraGeometryRef.current(),
  });
  const cameraWindow = cameraWin.window, setCameraWindow = cameraWin.setWindow;
  const refreshCameraGeometry = useCallback(async (): Promise<CameraGeometry | null> => {
    try {
      const geometry = await bridge.fetchCameraGeometry();
      setCameraGeometry(geometry);
      cameraWin.showDefault(geometry);
      return geometry;
    } catch {
      return null;
    }
  }, [cameraWin.showDefault]);
  refreshCameraGeometryRef.current = refreshCameraGeometry;

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
  return { cameraWin, cameraWindow, setCameraWindow, refreshCameraGeometry };
}
export type CameraWindowingModel = ReturnType<typeof useCameraWindowing>;

type ModeSwitchDeps = Pick<ShellState, "ready" | "tab" | "append" | "instrumentModes" | "instrumentRef" | "instrumentMode"
    | "instrument" | "runMode">
  & Pick<ConnectState, "cameraWindowRef" | "setModeError" | "modeRetry" | "modeError" | "setRunning">
  & Pick<MonitoringState, "expActive"> & CameraWindowingModel;

// Qt parity: the tab picks the camera mode (Align / full sensor on Overview, Run / the window on Experiment).
export function useCameraModeSwitch({
  ready, tab, append, instrumentModes, instrumentRef, instrumentMode, instrument, runMode, cameraWindowRef, setModeError,
  modeRetry, modeError, setRunning, expActive, cameraWin, refreshCameraGeometry,
}: ModeSwitchDeps) {
  // After a page reload the backend still holds the Run window it applied: take it back.
  useEffect(() => { cameraWin.restore(instrumentMode); }, [instrumentMode?.run_set, instrumentMode?.run_x, instrumentMode?.run_y, cameraWin.restore]);

  // Qt parity (MainWindow tab change): Camera & Alignment shows the whole sensor, Experiment the
  // saved window, other tabs leave the camera as it is; never during a run. A running capture
  // restarts in the new mode (backend).
  useEffect(() => {
    if (!ready || (tab !== "overview" && tab !== "experiment")) return;
    let cancelled = false;
    void (async () => {
      const geometry = await refreshCameraGeometry();
      if (cancelled || expActive) return;
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
      if (!geometry?.supported) return;
      const overview = tab === "overview";
      if (geometry.overview === overview) return;
      const result = await bridge.setCameraOverview(overview);
      append(result.ok ? result.message : `Camera mode: ${result.message}`);
      if (!cancelled) await refreshCameraGeometry();
    })();
    return () => { cancelled = true; };
  }, [tab, ready, expActive, refreshCameraGeometry, append, instrumentModes, cameraWin.placed, modeRetry, instrumentMode?.idle, instrument?.ring?.run_frozen]);

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

type ConnectModelDeps = Pick<ShellState, "ready" | "caps" | "instrument" | "pz7035" | "instrumentModes">
  & Pick<ConnectState, "camSelection" | "running" | "alignImage"> & Pick<ConfigState, "coreStatus" | "stats">
  & Pick<HardwareState, "afStatus" | "samplePump" | "sheathPump" | "trigStatus">
  & Pick<PreviewState, "backgroundSet" | "roiFields" | "lastMeta"> & CoresModel & Pick<CameraSetupModel, "cameraScript">;

// Render-time values: the Start Camera gate, the UX-3 preflight checklist and the UX-4 quality gates.
export function connectModel({
  ready, caps, instrument, pz7035, instrumentModes, camSelection, running, alignImage, coreStatus, stats, afStatus,
  samplePump, sheathPump, trigStatus, backgroundSet, roiFields, lastMeta, cores, cameraScript,
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
  return { cameraConfigured, startCameraReason, preflight, qualityInput, quality };
}
export type ConnectModel = ReturnType<typeof connectModel>;

