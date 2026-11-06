// Connect and Overview (epic #863): camera discovery and selection, the mock camera, camera
// start/stop, the hardware preflight checklist, the camera window and the Align/Run mode switch,
// and the Overview quality gates. Moved from App.tsx without change (#869 wave 0);
// studio/useStudioController.ts calls these hooks in App.tsx's original effect order.
import { useCallback, useEffect, useRef, useState } from "react";
import {
  bridge, type CameraDiscovery, type CameraGeometry, type CameraSelection, type CaptureLifecycle, type FrameDeliveryMode,
  type FrameDeliveryModeStatus, type RecordingCapacity, type RecordingTargetState,
} from "../../bridge";
import { CAMERA_SELECTION_MODES, PUMP_IDS } from "../../bridgeContract";
import { snapRunWindow, type Rect } from "../../cameraAlignment";
import { runModeBlockReason, useCameraWindow, type CameraModeError } from "../../cameraWindow";
import { useViewerOnly } from "../../components/ViewerGate";
import { DiscoveryRefusedError } from "../../discovery";
import { invoke, isRemote } from "../../transport";
import { derivePreflight, type PreflightInput } from "../../preflight";
import { readAutoSelect } from "../../startupPreference";
import { deriveQualityGates, type ImageQualityInput, type QualityInput } from "../../quality";
import type { RunFpsStatus } from "../../runFps";
import type { CameraSetupModel, ConfigDocumentsModel, ConfigState, CoresModel } from "../config/useConfig";
import type { HardwareState } from "../hardware/useHardware";
import type { MonitoringState } from "../monitoring/useMonitoring";
import type { LiveFramesModel, PreviewState } from "../preview/usePreview";
import type { ReviewState } from "../review/useReview";
import type { ShellState, StatePollingModel } from "../shell/useShell";
import {
  cameraPhase, connectedNote, defaultMockFolder, holdCameraCommand, rowOfSelection, discoveryProblemText, discoveryStatusText, isClickEvent, loadMockSettings,
  MOCK_CONFIGURED_NOTE, MOCK_FPS_DEFAULT, NO_PICK, parseMockFps, pickedRow, preselectDevices, saveMockSettings,
  startCameraBlockReason, stopCameraBlockReason, VANISHED_NOTE, type CameraCommand, type DeviceTab, type PickedDevices,
} from "./connectFlow";
import {
  isEgrabber, NO_SOURCES, resolveExperimentScript, runEgrabberStage, runOverviewAutoStart,
  type ScriptSources, type StageIo, type StageName, type StageNotice,
} from "./cameraStage";

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
  // Start / Stop Camera: one command in flight (CONNECT-033) and the capture lifecycle behind the phase text (CONNECT-034).
  const [cameraCommand, setCameraCommand] = useState<CameraCommand | null>(null);
  const cameraCommandRef = useRef<CameraCommand | null>(null);
  const [lifecycle, setLifecycle] = useState<CaptureLifecycle | null>(null);
  // What the preflight needs from outside the backend's camera state: the camera the active profile was made for
  // (App.tsx:1237) and the data directory's storage on the desktop (preflight.ts:360-363).
  const [expectedCamera, setExpectedCamera] = useState("");
  const [desktopStorage, setDesktopStorage] = useState<RecordingTargetState | null>(null);

  // Mock camera configuration modal.
  const [showMockConfig, setShowMockConfig] = useState(false);
  // The dialog's fields start from what the operator used last time (Qt's frame rate default is the rig's 5000 fps).
  const [frameDir, setFrameDir] = useState(() => loadMockSettings(localStorage).dir ?? "");
  const [mockFps, setMockFps] = useState(() => loadMockSettings(localStorage).fps ?? String(MOCK_FPS_DEFAULT));
  const [loopFiles, setLoopFiles] = useState(() => loadMockSettings(localStorage).loop ?? true);
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
  return {
    running, setRunning, camStatus, setCamStatus, connectTab, setConnectTab,
    discovery, setDiscovery, camSelection, setCamSelection, pickedDevices, setPickedDevices, scanning, setScanning,
    scanNote, setScanNote, actionNote, setActionNote, noCameraDialog, setNoCameraDialog, cameraNotice, setCameraNotice,
    deliveryModeStatus, setDeliveryModeStatus, autoSelectStored, setAutoSelectStored,
    showMockConfig, setShowMockConfig, frameDir, setFrameDir, mockFps, setMockFps, loopFiles, setLoopFiles,
    cameraCommand, setCameraCommand, cameraCommandRef, lifecycle, setLifecycle, expectedCamera, setExpectedCamera,
    desktopStorage, setDesktopStorage,
    liveCanvasRef, alignImage, setAlignImage, alignBestRef, alignImageMs,
    cameraGeometry, setCameraGeometry, cameraGeometryRef, cameraWindowRef, refreshCameraGeometryRef,
    modeError, setModeError, modeRetry, setModeRetry, runFpsStatus, setRunFpsStatus,
    stageNotice, setStageNotice, geometryError, setGeometryError, capacity, setCapacity, scriptSourcesRef,
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
  & Pick<ConnectState, "setDiscovery" | "setCamSelection" | "setShowMockConfig" | "showMockConfig" | "setCamStatus" | "frameDir"
    | "setFrameDir" | "mockFps" | "loopFiles" | "pickedDevices" | "setPickedDevices" | "connectTab" | "discovery" | "camSelection" | "running"
    | "setRunning" | "setScanning" | "setScanNote" | "setActionNote" | "setCameraNotice" | "setDeliveryModeStatus" | "setDesktopStorage"
    | "cameraCommandRef" | "setCameraCommand">
  & Pick<MonitoringState, "expState" | "expStatus">
  & Pick<ConfigState, "setCoreStatus"> & Pick<HardwareState, "setAfStatus" | "setSamplePump" | "setSheathPump" | "setTrigStatus">
  & Pick<ReviewState, "setReviewing"> & Pick<PreviewState, "recording" | "setRecording">
  & Pick<StatePollingModel, "tick"> & Pick<LiveFramesModel, "stopLoop">;

// Discovery and selection, the Preflight polling, the mock camera and Start/Stop Camera.
export function useCameraConnection({
  ready, tab, setTab, append, setDiscovery, setCamSelection, setShowMockConfig, showMockConfig, setCamStatus, frameDir, setFrameDir,
  mockFps, loopFiles, pickedDevices, setPickedDevices, connectTab, discovery, camSelection, running, setRunning, setCoreStatus,
  setAfStatus, setSamplePump, setSheathPump, setTrigStatus, setReviewing, recording, setRecording, tick, stopLoop, setScanning,
  setScanNote, setActionNote, setCameraNotice, setDeliveryModeStatus, setDesktopStorage, cameraCommandRef, setCameraCommand, expState, expStatus,
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
      const [sel, core, af, sample, sheath, trig, delivery, instrumentStatus] = await Promise.all([
        bridge.fetchCameraSelection(),
        bridge.fetchProcessingCoreStatus(),
        bridge.fetchAutofocusStatus(),
        bridge.fetchPumpStatus(PUMP_IDS.Sample),
        bridge.fetchPumpStatus(PUMP_IDS.Sheath),
        bridge.fetchTriggerStatus(),
        // The delivery mode follows a profile apply within one poll; a bridge without it leaves the control off.
        bridge.fetchFrameDeliveryMode().catch(() => null),
        // The recording target of the data directory (every server answers it, not only the PZ7035): the Storage check.
        bridge.fetchInstrumentStatus().catch(() => null),
      ]);
      setDeliveryModeStatus(delivery?.valid ? delivery : null);
      setDesktopStorage(instrumentStatus?.storage ?? null);
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

  // The dialog opens on the folder used last time, else the folder the backend's mock holds, else Documents (Qt:
  // ../data/mock_frames next to the app, else Documents: the desktop app ships no mock folder, CONNECT-014).
  const camSelectionRef = useRef(camSelection);
  camSelectionRef.current = camSelection;
  useEffect(() => {
    if (!showMockConfig || frameDir.trim()) return;
    let alive = true;
    void (async () => {
      let documents = "";
      try { documents = (await bridge.appPaths()).documents; } catch { /* no Documents folder: the field stays empty */ }
      const held = camSelectionRef.current;
      const backendFolder = held?.mode === CAMERA_SELECTION_MODES.Mock ? held.mock_frame_dir : "";
      if (alive) setFrameDir((now) => now.trim() ? now : defaultMockFolder(loadMockSettings(localStorage).dir ?? "", backendFolder, documents));
    })();
    return () => { alive = false; };
  }, [showMockConfig]);

  // The frame rate is Qt's spin box: fps, 1 to 10000, default 5000; the backend turns it into a whole-microsecond
  // interval (ABI next (#896, set at merge)), so the mock is no longer capped at 1000 fps (CONNECT-016). The folder is trimmed.
  const onConfigureMock = useCallback(async () => {
    const dir = frameDir.trim();
    const fps = parseMockFps(mockFps);
    if (!dir || fps === null) return;
    try {
      const cfg = await bridge.configureMockFps(dir, fps, loopFiles);
      if (!cfg.ok) return refuse("Configure Mock Camera", cfg.message);
      if (!saveMockSettings(localStorage, { dir, fps: mockFps, loop: loopFiles })) append("The mock camera settings could not be remembered.");
      setFrameDir(dir);
      setShowMockConfig(false);
      setCamStatus("configured (mock)");
      append(`mock camera configured (${dir}, ${fps} fps)`);
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
  }, [frameDir, mockFps, loopFiles, append, adoptSelection, refreshCameraState, refuse, setTab]);

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

  // One camera command at a time, as Qt's controller (CONNECT-033): a second click, key press or shortcut while the first is
  // in flight sends nothing. A refused or failed command is a modal warning titled "Camera" and status text (CONNECT-036).
  const expStateRef = useRef(expState);
  expStateRef.current = expState;
  const expFlushingRef = useRef(false);
  expFlushingRef.current = !!(expStatus?.valid && expStatus.flushing);
  const runningRef2 = useRef(running);
  runningRef2.current = running;
  const beginCommand = (kind: "start" | "stop") => {
    if (cameraCommandRef.current) return false;
    cameraCommandRef.current = kind;
    setCameraCommand(kind);
    return true;
  };
  const endCommand = () => { cameraCommandRef.current = null; setCameraCommand(null); };

  const onStartCamera = useCallback(async () => {
    if (!beginCommand("start")) return;
    try {
      setReviewing(false);
      const res = await bridge.startCapture();
      if (!res.ok) return refuse("Camera", res.message);
      setRunning(true);
      append("capture start requested; waiting for camera-ready confirmation");

      // Parity with Qt: a successful start lands the operator on Overview.
      setTab((t) => (t === "connect" ? "overview" : t));
    } catch (e) {
      refuse("Camera", String(e));
    } finally {
      endCommand();
    }
  }, [tick, append, stopLoop, refuse, setTab]);

  const onStopCamera = useCallback(async () => {
    if (!runningRef2.current) return;
    const blocked = stopCameraBlockReason({
      running: true, expState: expStateRef.current, flushing: expFlushingRef.current, commandInFlight: !!cameraCommandRef.current,
    });
    if (blocked) { if (!cameraCommandRef.current) refuse("Camera", blocked); return; }
    if (!beginCommand("stop")) return;
    try {
      // Decision (CONNECT-039): Qt refuses Stop while raw recording is active ("Stop recording first"); here Stop Camera
      // closes the recording first, so the file is finalised and nothing is lost, then stops the camera.
      const wasRecording = recording;
      if (recording) {
        const stopped = await bridge.stopRecording();
        if (!stopped.ok) return refuse("Camera", `Stop recording failed: ${stopped.message}`);
        setRecording(false);
      }
      const stopped = await bridge.stopCapture();
      // A teardown the device did not confirm is a failed command with the camera's message (CONNECT-040).
      if (!stopped.ok) return refuse("Camera", stopped.message);
      setRunning(false);
      append(wasRecording ? "recording finalised; capture stopped" : "capture stopped");
    } catch (e) {
      refuse("Camera", String(e));
    } finally {
      endCommand();
    }
  }, [recording, stopLoop, append, refuse]);
  return { refreshCameraState, refreshPreflight, onConfigureMock, onConnectPicked, onDeliveryModeChange, onStartCamera, onStopCamera, refuse };
}
export type CameraConnectionModel = ReturnType<typeof useCameraConnection>;

type WindowingDeps = Pick<ShellState, "ready" | "tab" | "hostProcessing" | "instrumentModes" | "append">
  & Pick<ConnectState, "cameraGeometryRef" | "cameraWindowRef" | "refreshCameraGeometryRef" | "setCameraGeometry" | "setRunFpsStatus"
    | "scriptSourcesRef" | "setGeometryError" | "setCapacity">
  & Pick<MonitoringState, "expActive">;

/** Whether a script file can be read: an existing .js file the camera document command opens. A remote
 *  (browser) client cannot tell: the server reads no script file by path (`camera_document_confined` serves
 *  only the PZ7035 document), so it answers false without asking and the stage change says why. It never
 *  answers true for a file it cannot see: a stop, then a failed apply, would leave the camera stopped. */
export async function scriptExists(path: string, remote: boolean = isRemote): Promise<boolean> {
  if (remote) return false;
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
  setRunFpsStatus, expActive, scriptSourcesRef, setGeometryError, setCapacity,
}: WindowingDeps) {
  const cameraWin = useCameraWindow({
    instrumentModes, geometryRef: cameraGeometryRef, expActive, append, windowRef: cameraWindowRef,
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
  return { cameraWin, cameraWindow, setCameraWindow, refreshCameraGeometry };
}
export type CameraWindowingModel = ReturnType<typeof useCameraWindowing>;

type ModeSwitchDeps = Pick<ShellState, "ready" | "tab" | "tabRef" | "append" | "instrumentModes" | "instrumentRef" | "instrumentMode"
    | "instrument" | "runMode">
  & Pick<ConnectState, "cameraWindowRef" | "setModeError" | "modeRetry" | "modeError" | "setRunning" | "scriptSourcesRef" | "setStageNotice"
    | "cameraCommandRef" | "setCameraCommand">
  & Pick<MonitoringState, "expActive"> & Pick<PreviewState, "recording"> & Pick<ReviewState, "setReviewing"> & CameraWindowingModel;

// Qt parity: the tab picks the camera mode (Align / full sensor on Overview, Run / the window on Experiment).
export function useCameraModeSwitch({
  ready, tab, tabRef, append, instrumentModes, instrumentRef, instrumentMode, instrument, runMode, cameraWindowRef, setModeError,
  modeRetry, modeError, setRunning, expActive, recording, setReviewing, cameraWin, refreshCameraGeometry, scriptSourcesRef, setStageNotice,
  cameraCommandRef, setCameraCommand,
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
  // The stage change stops, reconfigures and starts the camera itself: it holds Start / Stop Camera's one-command guard
  // (CONNECT-033) so a click or the space bar cannot start the camera between its stop and its script apply.
  const commandGuard = { ref: cameraCommandRef, set: setCameraCommand };
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
          remote: isRemote,
          setRunning,
          log: append,
        };
        // Serialised, and dropped when the operator has moved on before its turn: a script apply is not interrupted.
        stageQueue.current = stageQueue.current.then(async () => {
          const notice = await holdCameraCommand(commandGuard, async () => {
            if (tabRef.current !== stage) return undefined;
            return runEgrabberStage({
              stage, expActive: live.current.expActive, recording: live.current.recording, sources: scriptSourcesRef.current, geometry,
            }, io);
          });
          if (notice !== undefined) setStageNotice(notice);
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
        const notice = await holdCameraCommand(commandGuard, async () => {
          if (tabRef.current !== "overview") return undefined;
          return runOverviewAutoStart({ expActive: live.current.expActive, geometry: fresh, instrumentModes }, {
            fetchSelection: () => bridge.fetchCameraSelection(),
            startCapture: async () => { setReviewing(false); return bridge.startCapture(); },
            setRunning,
            log: append,
          });
        });
        if (notice !== undefined) setStageNotice(notice);
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
export function syncScriptSources({ scriptSourcesRef, cameraScript, scriptDocument, profiles }:
  Pick<ConnectState, "scriptSourcesRef"> & Pick<CameraSetupModel, "cameraScript" | "scriptDocument"> & Pick<ConfigDocumentsModel, "profiles">) {
  scriptSourcesRef.current = {
    ...scriptSourcesRef.current,
    cameraScriptPath: cameraScript.path,
    cameraDocumentPath: scriptDocument.doc?.path ?? "",
    profileConfigPath: profiles.activeProfile?.path ?? "",
  };
}

type ConnectModelDeps = Pick<ShellState, "ready" | "caps" | "instrument" | "pz7035" | "instrumentModes">
  & Pick<ConnectState, "camSelection" | "running" | "alignImage" | "discovery" | "lifecycle" | "cameraCommand" | "expectedCamera"
    | "desktopStorage"> & Pick<ConfigState, "coreStatus" | "stats">
  & Pick<HardwareState, "afStatus" | "samplePump" | "sheathPump" | "trigStatus">
  & Pick<PreviewState, "backgroundSet" | "roiFields" | "lastMeta"> & CoresModel & Pick<CameraSetupModel, "cameraScript">;

// Render-time values: the Start Camera gate, the UX-3 preflight checklist and the UX-4 quality gates.
export function connectModel({
  ready, caps, instrument, pz7035, instrumentModes, camSelection, running, alignImage, coreStatus, stats, afStatus,
  samplePump, sheathPump, trigStatus, backgroundSet, roiFields, lastMeta, cores, cameraScript, discovery, lifecycle, cameraCommand,
  expectedCamera, desktopStorage,
}: ConnectModelDeps) {
  const cameraConfigured = camSelection?.configured ?? false;
  const phase = cameraPhase(lifecycle, cameraConfigured, running);
  const startCameraReason = startCameraBlockReason({
    updateBusy: cores.appUpdateBusy, scriptBusy: cameraScript.busy, ready, configured: cameraConfigured, running, phase: phase.phase,
    commandInFlight: cameraCommand !== null,
  });
  // The camera the profile was made for (camera.window.camera_model) against the one selected; a mock source has no model
  // to compare, so it is skipped (App.tsx:1237). The vendor is not compared: discovery reports none.
  const selectedRow = rowOfSelection(discovery, camSelection);
  const cameraModel = selectedRow && selectedRow.tab !== "framegrabbers" ? selectedRow.camera.model_name : "";
  const expectedIdentity = camSelection?.mode === CAMERA_SELECTION_MODES.Mock ? "" : expectedCamera;
  // The storage the preflight shows: the PZ7035's recording target, else the data directory the backend reports on every server.
  const storage = instrument?.storage ?? desktopStorage ?? undefined;

  // ---- UX-3 profile-aware hardware preflight (issue #307) ----
  const preflightInput: PreflightInput = {
    backendReady: ready,
    cameraConfigured,
    cameraRunning: running,
    cameraIdentity: camSelection?.label || (camSelection?.mode === 1 ? "Mock camera" : ""),
    cameraExpected: expectedIdentity,
    cameraModel,
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
    storageKnown: !!storage,
    storageWritable: storage?.writable ?? false,
    storageFreeOk: (storage?.free_bytes ?? 0) >= 1e9,
    storagePath: storage?.path ?? "",
    storageWarning: storage?.warning ?? "",
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
  return { cameraConfigured, startCameraReason, cameraPhase: phase, preflight, qualityInput, quality };
}
export type ConnectModel = ReturnType<typeof connectModel>;

