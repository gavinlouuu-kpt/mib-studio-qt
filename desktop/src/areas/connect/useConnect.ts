// Connect and Overview (epic #863): camera discovery and selection, the mock camera, camera
// start/stop, the hardware preflight checklist, the camera window and the Align/Run mode switch,
// and the Overview quality gates. Moved from App.tsx without change (#869 wave 0);
// studio/useStudioController.ts calls these hooks in App.tsx's original effect order.
import { useCallback, useEffect, useRef, useState } from "react";
import {
  bridge, type CameraDiscovery, type CameraGeometry, type CameraSelection, type FrameDeliveryMode, type FrameDeliveryModeStatus,
} from "../../bridge";
import { PUMP_IDS } from "../../bridgeContract";
import { snapRunWindow, type Rect } from "../../cameraAlignment";
import { runModeBlockReason, useCameraWindow, type CameraModeError } from "../../cameraWindow";
import { DiscoveryRefusedError } from "../../discovery";
import { derivePreflight, type PreflightInput } from "../../preflight";
import { readAutoSelect } from "../../startupPreference";
import { deriveQualityGates, type ImageQualityInput, type QualityInput } from "../../quality";
import type { RunFpsStatus } from "../../runFps";
import type { CameraSetupModel, ConfigState, CoresModel } from "../config/useConfig";
import type { HardwareState } from "../hardware/useHardware";
import type { MonitoringState } from "../monitoring/useMonitoring";
import type { LiveFramesModel, PreviewState } from "../preview/usePreview";
import type { ReviewState } from "../review/useReview";
import type { ShellState, StatePollingModel } from "../shell/useShell";
import {
  connectedNote, discoveryProblemText, discoveryStatusText, isClickEvent, MOCK_CONFIGURED_NOTE, NO_PICK, pickedRow, preselectDevices,
  VANISHED_NOTE, type DeviceTab, type PickedDevices,
} from "./connectFlow";

export function useConnectState() {
  const [running, setRunning] = useState(false);
  const [camStatus, setCamStatus] = useState("unconfigured");
  const [connectTab, setConnectTab] = useState<"cameras" | "mindvision" | "framegrabbers">("cameras");

  // Camera discovery/selection (bridge schema v7, BE-2). The selection
  // snapshot from the backend is authoritative — no local mirror of it.
  const [discovery, setDiscovery] = useState<CameraDiscovery | null>(null);
  const [camSelection, setCamSelection] = useState<CameraSelection | null>(null);
  // One picked row per sub-tab (Qt: one list each); Connect acts on the shown sub-tab's row (#863 CONNECT-002/003).
  const [pickedDevices, setPickedDevices] = useState<PickedDevices>(NO_PICK);
  // The Connect status line (Qt's statusLabel): a scan in flight, the last scan's result, the last action's result.
  const [scanning, setScanning] = useState(false);
  const [scanNote, setScanNote] = useState("");
  const [actionNote, setActionNote] = useState("");
  // "No camera found" dialog (CONNECT-024) and the modal warning of a refused camera command ("Camera", "Connect Device").
  const [noCameraDialog, setNoCameraDialog] = useState(false);
  const [cameraNotice, setCameraNotice] = useState<{ title: string; message: string } | null>(null);
  // The capture delivery mode (CONNECT-019); the operator's startup auto-select choice (null = not chosen: the default).
  const [deliveryModeStatus, setDeliveryModeStatus] = useState<FrameDeliveryModeStatus | null>(null);
  const [autoSelectStored, setAutoSelectStored] = useState<boolean | null>(() => readAutoSelect(localStorage));

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
    discovery, setDiscovery, camSelection, setCamSelection, pickedDevices, setPickedDevices, scanning, setScanning,
    scanNote, setScanNote, actionNote, setActionNote, noCameraDialog, setNoCameraDialog, cameraNotice, setCameraNotice,
    deliveryModeStatus, setDeliveryModeStatus, autoSelectStored, setAutoSelectStored,
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
    | "loopFiles" | "pickedDevices" | "setPickedDevices" | "connectTab" | "discovery" | "running" | "setRunning" | "setScanning" | "setScanNote"
    | "setActionNote" | "setCameraNotice" | "setDeliveryModeStatus">
  & Pick<ConfigState, "setCoreStatus"> & Pick<HardwareState, "setAfStatus" | "setSamplePump" | "setSheathPump" | "setTrigStatus">
  & Pick<ReviewState, "setReviewing"> & Pick<PreviewState, "recording" | "setRecording">
  & Pick<StatePollingModel, "tick"> & Pick<LiveFramesModel, "stopLoop">;

// Discovery and selection, the Preflight polling, the mock camera and Start/Stop Camera.
export function useCameraConnection({
  ready, tab, setTab, append, setDiscovery, setCamSelection, setShowMockConfig, setCamStatus, frameDir, intervalMs,
  loopFiles, pickedDevices, setPickedDevices, connectTab, discovery, running, setRunning, setCoreStatus, setAfStatus, setSamplePump,
  setSheathPump, setTrigStatus, setReviewing, recording, setRecording, tick, stopLoop, setScanning, setScanNote,
  setActionNote, setCameraNotice, setDeliveryModeStatus,
}: ConnectionDeps) {
  // ---- Camera discovery/selection (BE-2) + camera actions ----

  // One scan at a time: a second Refresh (or a refresh after Connect) joins the scan in flight, as Qt's Refresh joins a
  // running discovery job. A cancelled, incomplete or refused scan says so on the status line; it is never "0 cameras
  // found" (#863 CONNECT-004/005/007). The lists are re-fetched once per scan, with row 0 preselected.
  const scanRef = useRef<Promise<void> | null>(null);
  const runningRef = useRef(running);
  runningRef.current = running;
  // The Refresh button passes its click event: that scan starts from a clean status line (Qt's onRefresh overwrites the
  // label); the refreshes other code asks for (after Connect, after a startup selection, after a script apply) keep
  // the last action's result ("Connected to ...") on the line.
  const refreshCameraState = useCallback((from?: unknown): Promise<void> => {
    if (scanRef.current) return scanRef.current;
    const operator = isClickEvent(from);
    if (!operator && runningRef.current) {
      // The backend refuses a scan under a running capture; a refresh the code asked for (not the operator) only
      // re-reads the selection then, so the Overview's live view never causes a "discovery refused" line.
      return bridge.fetchCameraSelection().then(setCamSelection).catch((e) => append(`discovery error: ${e}`));
    }
    const run = (async () => {
      setScanning(true);
      if (operator) setActionNote("");
      try {
        const found = await bridge.discoverCameras();
        setDiscovery(found);
        setPickedDevices((previous) => preselectDevices(found, previous));
        setScanNote(discoveryStatusText(found));
      } catch (e) {
        const reason = e instanceof DiscoveryRefusedError ? e.reason : e instanceof Error ? e.message : String(e);
        setScanNote(discoveryProblemText(reason));
        append(`discovery error: ${e}`);
      }
      try {
        setCamSelection(await bridge.fetchCameraSelection());
      } catch (e) {
        append(`discovery error: ${e}`);
      } finally {
        setScanning(false);
        scanRef.current = null;
      }
    })();
    scanRef.current = run;
    return run;
  }, [append]);

  useEffect(() => {
    if (ready) void refreshCameraState();
  }, [ready, refreshCameraState]);

  // UX-3: poll every subsystem the preflight checklist needs. Runs off the
  // capture loop so preflight works before the camera is started.
  const refreshPreflight = useCallback(async () => {
    try {
      const [sel, core, af, sample, sheath, trig, delivery] = await Promise.all([
        bridge.fetchCameraSelection(),
        bridge.fetchProcessingCoreStatus(),
        bridge.fetchAutofocusStatus(),
        bridge.fetchPumpStatus(PUMP_IDS.Sample),
        bridge.fetchPumpStatus(PUMP_IDS.Sheath),
        bridge.fetchTriggerStatus(),
        // The delivery mode follows a profile apply within one poll; a bridge without it leaves the control off.
        bridge.fetchFrameDeliveryMode().catch(() => null),
      ]);
      setDeliveryModeStatus(delivery?.valid ? delivery : null);
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

  // A refused camera command is a modal warning as well as status text (Qt: QMessageBox::warning), not only a log line.
  const refuse = useCallback((title: string, message: string) => {
    setActionNote(message);
    setCameraNotice({ title, message });
    append(`${title}: ${message}`);
  }, [append]);

  // The backend's selection is read first and the stage moves on at once: the device scan that refreshes the lists runs
  // behind it (it takes seconds on a real rig), while Start Camera on the next stage already sees the new selection.
  const adoptSelection = useCallback(async () => {
    try {
      setCamSelection(await bridge.fetchCameraSelection());
    } catch (e) {
      append(`discovery error: ${e}`);
    }
  }, [append]);

  const onConfigureMock = useCallback(async () => {
    try {
      const cfg = await bridge.configureMock(frameDir, Number(intervalMs) || 33, loopFiles);
      if (!cfg.ok) return refuse("Configure Mock Camera", cfg.message);
      setShowMockConfig(false);
      setCamStatus("configured (mock)");
      append(`mock camera configured (${frameDir})`);
      await adoptSelection();
      // Qt: "Mock camera configured (not capturing)" and the stage moves to Overview (CONNECT-017); the Overview stage
      // applies its script and starts the live view.
      setActionNote(MOCK_CONFIGURED_NOTE);
      setTab((t) => (t === "connect" ? "overview" : t));
      void refreshCameraState();
    } catch (e) {
      setShowMockConfig(false);
      refuse("Configure Mock Camera", String(e));
    }
  }, [frameDir, intervalMs, loopFiles, append, adoptSelection, refreshCameraState, refuse, setTab]);

  // Connect acts on the row picked in the shown sub-tab; a framegrabber row selects that interface/device
  // (CONNECT-003). A picked row that a rescan removed says so instead of doing nothing (App.tsx:703).
  const onConnectPicked = useCallback(async () => {
    if (!discovery) return;
    const tabPick: DeviceTab = connectTab;
    const row = pickedRow(discovery, tabPick, pickedDevices);
    if (!row) {
      if (pickedDevices[tabPick]) {
        setActionNote(VANISHED_NOTE);
        append(VANISHED_NOTE);
        void refreshCameraState();
      }
      return;
    }
    try {
      let res: { ok: boolean; message: string };
      if (row.tab === "framegrabbers") {
        const g = row.framegrabber;
        res = await bridge.selectHardwareCamera(g.interface_index, g.device_index, g.label);
      } else if (row.camera.camera_type === 2) {
        // Mock source: configuration happens through the modal.
        setShowMockConfig(true);
        return;
      } else if (row.camera.camera_type === 1) {
        res = await bridge.selectMindVisionCamera(row.camera.camera_index, row.camera.label, "");
      } else {
        res = await bridge.selectHardwareCamera(row.camera.interface_index, row.camera.device_index, row.camera.label);
      }
      if (!res.ok) return refuse("Connect Device", res.message);
      append(`selected ${row.label}`);
      await adoptSelection();
      // Qt (CONNECT-011): "Connected to <label> (not capturing)" and the stage moves to Overview.
      setActionNote(connectedNote(row.label));
      setTab((t) => (t === "connect" ? "overview" : t));
      void refreshCameraState();
    } catch (e) {
      refuse("Connect Device", String(e));
    }
  }, [pickedDevices, connectTab, discovery, append, adoptSelection, refreshCameraState, refuse, setTab]);

  // The delivery mode (CONNECT-019/020): applies at the next capture start; the backend's reply says which.
  const onDeliveryModeChange = useCallback(async (mode: FrameDeliveryMode) => {
    try {
      const result = await bridge.setFrameDeliveryMode(mode);
      setActionNote(result.message);
      append(result.message);
      const status = await bridge.fetchFrameDeliveryMode();
      setDeliveryModeStatus(status.valid ? status : null);
    } catch (e) {
      setActionNote(`Delivery mode: ${e}`);
      append(`delivery mode error: ${e}`);
    }
  }, [append]);

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
  return { refreshCameraState, refreshPreflight, onConfigureMock, onConnectPicked, onDeliveryModeChange, onStartCamera, onStopCamera };
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

