// Shell (epic #862): backend start-up and recovery, the log, the stage tab, platform and
// instrument status, state reconciliation, shell preferences and the close guard. Moved from
// App.tsx without change (#869 wave 0); studio/useStudioController.ts calls these hooks in
// App.tsx's original effect order. Owners: docs/howto/desktop-area-ownership.md.
import { getVersion } from "@tauri-apps/api/app";
import { useCallback, useEffect, useRef, useState } from "react";
import desktopPackage from "../../../package.json";
import { useCloseGuard } from "../../closeGuard";
import { bridge, type BridgeEvent, type InstrumentStatus, type PlatformInfo } from "../../bridge";
import { BRIDGE_ABI_VERSION, COMMAND_TYPES, EXPERIMENT_STATES } from "../../bridgeContract";
import { publishAutofocusMessage } from "../../components/autofocusMessages";
import { productName } from "../../brand";
import { isString, isStringArray, usePersistedState } from "../../persistedState";
import { capabilitiesOf, isPz7035 } from "../../platformCapabilities";
import { faultClearedNote } from "../../ringPlayback";
import { resultsRates, type ResultsRates, type ResultsSample } from "../../resultsRates";
import { recoverNativeRuntime } from "../../runtimeRecovery";
import { invoke, isRemote } from "../../transport";
import { setRemoteDefaultDir } from "../../transport/dialogs";
import type { ConfigDocumentsModel, CameraSetupModel, ConfigState, CoresModel } from "../config/useConfig";
import type { ConnectState } from "../connect/useConnect";
import type { HardwareState } from "../hardware/useHardware";
import type { MonitoringState } from "../monitoring/useMonitoring";
import type { PreviewBufferModel, PreviewState } from "../preview/usePreview";
import type { ReviewJobsModel, ReviewState } from "../review/useReview";

export const SIDEBAR_KEY = "mib.sidebar.collapsed";

export type MainTab = "connect" | "overview" | "experiment" | "review";

export function useShellState() {
  const [abi, setAbi] = useState<number | null>(null);
  const [ready, setReady] = useState(false);
  // The log and the stage confirmations survive a reload in this tab (#550 m14).
  const [log, setLog] = usePersistedState<string[]>("yofo.log", [], isStringArray);
  const [showLog, setShowLog] = useState(false);

  // Shell state.
  const [tab, setTab] = useState<MainTab>("connect");
  // UX-1 guided workflow: explicit operator confirmations, stored as the
  // device/core signature they were confirmed against so they auto-invalidate
  // when the hardware changes. Kept in memory (not persisted) so every session
  // re-confirms readiness. `didInitStage` guards the one-time startup landing.
  const [preflightConfirmedFor, setPreflightConfirmedFor] = usePersistedState("yofo.preflightConfirmedFor", "", isString);
  const [alignmentConfirmedFor, setAlignmentConfirmedFor] = usePersistedState("yofo.alignmentConfirmedFor", "", isString);
  const didInitStage = useRef(false);
  const [sidebarCollapsed, setSidebarCollapsed] = useState(
    () => localStorage.getItem(SIDEBAR_KEY) === "1",
  );
  const [offlineHelp, setOfflineHelp] = useState<"notes" | "manual" | null>(null);
  const [helpVersion, setHelpVersion] = useState(desktopPackage.version);
  const openOfflineHelp = (kind: "notes" | "manual") => {
    void getVersion().then(setHelpVersion).catch(() => {});
    setOfflineHelp(kind);
  };
  const [showAbout, setShowAbout] = useState(false);
  const [showDiagnostics, setShowDiagnostics] = useState(false);
  const [showCentralMethods, setShowCentralMethods] = useState(false);
  const [resumedNative,setResumedNative]=useState(false);
  const tickBusy = useRef(false);
  const tabRef = useRef<MainTab>("connect");
  tabRef.current = tab;
  // Where the science runs (ABI 21). On the PZ7035 the PL processes every frame and the host
  // pipeline's controls (realtime switch, backgrounds, calibration, processed preview) do not apply.
  const [platform, setPlatform] = useState<PlatformInfo | null>(null);
  const hostProcessing = platform ? platform.host_processing : true;
  // #501: surfaces follow what the instrument has; the PZ7035 reports its PL core and health.
  const caps = capabilitiesOf(platform);
  const pz7035 = isPz7035(caps);
  // Backend-owned camera modes (#501 P1): Camera & Alignment = Align, Experiment = Run.
  const instrumentModes = caps.align_mode && caps.run_mode;
  const instrumentModesRef = useRef(false);
  instrumentModesRef.current = instrumentModes;
  // YOFO Studio on the PZ7035 and in the remote browser UI, MIB Studio on the desktop (#550 m13).
  const brand = productName({ pz7035, remote: isRemote });
  const [instrument, setInstrument] = useState<InstrumentStatus | null>(null);
  const instrumentModeName = instrument?.mode?.name;
  const [resultsNow, setResultsNow] = useState<ResultsRates | null>(null);
  const lastResultsSample = useRef<ResultsSample | null>(null);
  const instrumentRef = useRef<InstrumentStatus | null>(null);
  instrumentRef.current = instrument;
  const runMode = instrument?.mode?.name === "run";
  // Run stopped to review the frame ring (#649 v1): the cell-capture preview pauses and the playback takes its place.
  const ring = instrument?.ring;
  const runFrozen = runMode && !!ring?.run_frozen;
  // A PL fault that the Run start cleared automatically is never hidden: said once in the log, and kept on the run card and in Diagnostics.
  const faultNote = faultClearedNote(ring);

  const append = useCallback((line: string) => {
    setLog((l) => [`${new Date().toLocaleTimeString()} ${line}`, ...l].slice(0, 50));
    // Shell-side log sink (BE-9): every drawer line also lands in
    // <app_log>/desktop-shell.log for correlation with the backend logs.
    void bridge.shellLog("info", line).catch(() => {});
  }, []);
  const instrumentMode = instrument?.mode;
  const instrumentDataDir = instrument?.storage?.path ?? "";
  return {
    abi, setAbi, ready, setReady, log, setLog, showLog, setShowLog, tab, setTab,
    preflightConfirmedFor, setPreflightConfirmedFor, alignmentConfirmedFor, setAlignmentConfirmedFor, didInitStage,
    sidebarCollapsed, setSidebarCollapsed, offlineHelp, setOfflineHelp, helpVersion, openOfflineHelp,
    showAbout, setShowAbout, showDiagnostics, setShowDiagnostics, showCentralMethods, setShowCentralMethods,
    resumedNative, setResumedNative, tickBusy, tabRef,
    platform, setPlatform, hostProcessing, caps, pz7035, instrumentModes, instrumentModesRef, brand,
    instrument, setInstrument, instrumentModeName, resultsNow, setResultsNow, lastResultsSample, instrumentRef,
    runMode, ring, runFrozen, faultNote, append, instrumentMode, instrumentDataDir,
  };
}
export type ShellState = ReturnType<typeof useShellState>;

export function useDocumentTitle(brand: string) {
  useEffect(() => { document.title = brand; }, [brand]);
}

type StartupDeps = Pick<ShellState, "faultNote" | "append" | "setAbi" | "setResumedNative" | "setTab" | "setReady">
  & Pick<ConnectState, "setRunning"> & Pick<PreviewState, "setRecording"> & Pick<MonitoringState, "setExpStatus">
  & Pick<ReviewState, "setReviewMeta" | "setReviewPath" | "setReviewing" | "setReviewTab" | "recoveredReview">;

// The fault-cleared log line, then the backend initialization and native-session recovery.
export function useShellStartup({
  faultNote, append, setAbi, setResumedNative, setTab, setReady, setRunning, setRecording, setExpStatus,
  setReviewMeta, setReviewPath, setReviewing, setReviewTab, recoveredReview,
}: StartupDeps) {
  const lastFaultNote = useRef("");
  useEffect(() => {
    if (faultNote && faultNote !== lastFaultNote.current) append(faultNote);
    lastFaultNote.current = faultNote;
  }, [faultNote, append]);

  // Initialize the backend on boot (empty data dir resolves to Tauri's
  // app_data_dir on the Rust side) — the Qt app has no manual init step.
  useEffect(() => {
    bridge
      .abiVersion()
      .then((v) => {
        setAbi(v);
        if (v < BRIDGE_ABI_VERSION) {
          append(`bridge ABI ${v} is older than the UI contract (${BRIDGE_ABI_VERSION})`);
        }
      })
      .catch((e) => append(`abi error: ${e}`));
    (async () => {
      try {
        const snapshot=await recoverNativeRuntime();
        setResumedNative(snapshot.resumed);
        setRunning(snapshot.runtime.capture_running);setRecording(snapshot.runtime.recording);
        setExpStatus(snapshot.experiment);setReviewMeta(snapshot.review);
        if(snapshot.review.file_open){
          setReviewPath(snapshot.review.file_path);setReviewing(true);
          setReviewTab(snapshot.review.recording_file?"raw":"valid");
          recoveredReview.current=true;
          if(!snapshot.runtime.capture_running)setTab("review");
        }
        setReady(true);
        append(snapshot.resumed ? "native session recovered without replaying startup settings" : "backend initialized");
      } catch (e) {
        append(`init error: ${e}`);
      }
    })();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
}

type PollingDeps = Pick<ShellState, "append" | "ready" | "tickBusy"> & Pick<ConnectState, "setCamStatus" | "setRunning">
  & Pick<ReviewState, "setRange"> & Pick<PreviewState, "setRecording"> & Pick<ConfigState, "setStats">
  & Pick<MonitoringState, "setExpStatus"> & Pick<HardwareState, "setAfStatus" | "setAfStaleMs">;

// Backend notifications and the 500 ms state reconciliation tick.
export function useStatePolling({
  append, ready, tickBusy, setCamStatus, setRunning, setRange, setRecording, setStats, setExpStatus, setAfStatus, setAfStaleMs,
}: PollingDeps) {
  const applyEvents = useCallback((events: BridgeEvent[]) => {
    for (const e of events) {
      if (e.kind === "CameraStatus") {
        setCamStatus(`${e.running ? "running" : e.configured ? "configured" : "unconfigured"} (${e.label || "camera"})`);
      } else if (e.kind === "PlaybackPosition") {
        setRange({earliest:e.earliest,latest:e.latest,count:e.available});
      } else if (e.kind === "BackendError") {
        append(`backend error: ${e.message}${e.textTruncated ? " (details truncated)" : ""}`);
        // HW-033 (#868): the nanopositioner panel shows the autofocus failures as its status line.
        if (e.command === COMMAND_TYPES.Autofocus) publishAutofocusMessage(e.message);
      } else if (e.kind === "OperationStatus") {
        if (e.state === 2) append(`operation ${e.operationId} completed: ${e.message}`);
        else if (e.state >= 3) append(`operation ${e.operationId} ${e.state === 3 ? "failed" : e.state === 4 ? "cancelled" : "timed out"}: ${e.message}`);
      } else if (e.kind === "QueueOverflow") {
        append(`notification loss: ${e.notificationsDropped} discarded (total ${e.notificationsDroppedTotal}); authoritative reconciliation required`);
      } else if (e.kind === "ExperimentStatus") {
        if(e.state === EXPERIMENT_STATES.Failed) append(`experiment failed: ${e.message}`);
        else if(e.message) append(`experiment: ${e.message}`);
        // Regular bounded status polling retrieves current truth independently.
      } else if(e.kind === "Unknown") {
        append(`unrecognized notification ${e.receivedKind}; authoritative reconciliation required`);
      }
    }
  },[append]);

  const configTicks = useRef(0);
  const tick = useCallback(async () => {
    if (tickBusy.current) return;
    tickBusy.current = true;
    try {
      // State/fault notifications do not wait behind pixel decode/rendering.
      applyEvents(await bridge.pollEvents());
      const runtime = await invoke<{capture_running: boolean; recording: boolean}>("fetch_preview_buffer");
      setRunning(runtime.capture_running); setRecording(runtime.recording);
      setStats(await bridge.fetchProcessingStats());
      setExpStatus(await bridge.fetchExperimentStatus());
      setAfStatus(await bridge.fetchAutofocusStatus());
      // HW-034 (#868): the sidebar dims the ring width at the configured staleness limit (read every 10th tick).
      if (configTicks.current++ % 10 === 0) {
        try {
          const af = await bridge.fetchAutofocusConfig();
          if (af.valid && af.ring_ratio_stale_ms > 0) setAfStaleMs(af.ring_ratio_stale_ms);
        } catch { /* keep the last known limit; the status read above is what the tick reports */ }
      }
    } catch (e) {
      append(`tick error: ${e}`);
    } finally { tickBusy.current = false; }
  }, [applyEvents, append]);

  // State reconciliation belongs to the shell, not a capture button/view.
  // Remains active after stop/finalization and recovers after webview reload.
  useEffect(() => {
    if (!ready) return;
    void tick();
    const timer = window.setInterval(() => void tick(), 500);
    return () => window.clearInterval(timer);
  }, [ready, tick]);
  return { applyEvents, tick };
}
export type StatePollingModel = ReturnType<typeof useStatePolling>;

export function useShellPreferences({ setSidebarCollapsed }: Pick<ShellState, "setSidebarCollapsed">) {
  const toggleSidebar = useCallback(() => {
    setSidebarCollapsed((c) => {
      const next = !c;
      localStorage.setItem(SIDEBAR_KEY, next ? "1" : "0");
      // Durable copy in the shell preferences file (BE-9) — survives
      // webview-storage clearing.
      void bridge
        .getPreferences()
        .then((prefs) => bridge.setPreferences({ ...prefs, sidebarCollapsed: next }))
        .catch(() => {});
      return next;
    });
  }, []);

  // Restore persisted shell preferences at boot (BE-9).
  useEffect(() => {
    bridge
      .getPreferences()
      .then((prefs) => {
        if (typeof prefs.sidebarCollapsed === "boolean") {
          setSidebarCollapsed(prefs.sidebarCollapsed);
          localStorage.setItem(SIDEBAR_KEY, prefs.sidebarCollapsed ? "1" : "0");
        }
      })
      .catch(() => {});
  }, []);
  return { toggleSidebar };
}

type PlatformDeps = Pick<ShellState, "ready" | "caps" | "setPlatform" | "setInstrument" | "setResultsNow" | "lastResultsSample">;

export function usePlatformStatus({ ready, caps, setPlatform, setInstrument, setResultsNow, lastResultsSample }: PlatformDeps) {
  useEffect(() => {
    if (!ready) return;
    void bridge.fetchPlatformInfo().then(setPlatform).catch(() => setPlatform(null));
  }, [ready]);

  // PZ7035 identity and health, once a second (link rates need two samples).
  useEffect(() => {
    if (!ready || !caps.pl_identity) { setInstrument(null); return; }
    let live = true;
    const poll = () => void bridge.fetchInstrumentStatus().then((s) => {
      if (!live) return;
      // Result-stream rates from successive polls (#501); only while the provider runs.
      const r = s.results;
      if (r?.available && r.running) {
        const cur: ResultsSample = {
          atMs: performance.now(), frames: r.frames ?? 0, results: r.results ?? 0, empty_frames: r.empty_frames ?? 0,
          invalid_frames: r.invalid_frames ?? 0, truncated_frames: r.truncated_frames ?? 0,
        };
        setResultsNow(resultsRates(lastResultsSample.current, cur));
        lastResultsSample.current = cur;
      } else {
        lastResultsSample.current = null;
        setResultsNow(null);
      }
      setInstrument(s);
    }).catch((e) => { if (live) setInstrument({ available: false, error: String(e) }); });
    poll();
    const id = window.setInterval(poll, 1000);
    return () => { live = false; window.clearInterval(id); };
  }, [ready, caps.pl_identity]);
}

export function useRemoteDefaultDir(instrumentDataDir: string) {
  // Relative default save paths in the browser start under the instrument's data directory (G8).
  useEffect(() => { setRemoteDefaultDir(instrumentDataDir); }, [instrumentDataDir]);
}

type CloseGuardDeps = Pick<ShellState, "ready" | "append" | "setShowLog"> & Pick<ConfigState, "configDirty" | "quickDraft">
  & Pick<MonitoringState, "experimentRequestBusy"> & Pick<ReviewState, "reviewSourceBusy">
  & CameraSetupModel & CoresModel & ConfigDocumentsModel & ReviewJobsModel & PreviewBufferModel;

export function useShellCloseGuard({
  ready, append, setShowLog, configDirty, quickDraft, experimentRequestBusy, reviewSourceBusy,
  scriptDocument, mindvisionDocument, cameraScript, cores, checkedConfig, profiles, reviewExport, reanalysis, previewBuffer,
}: CloseGuardDeps) {
  const requestClose = useCloseGuard({ready, busy:scriptDocument.busy || mindvisionDocument.busy || reviewSourceBusy || experimentRequestBusy || cameraScript.busy || cores.busy || checkedConfig.busy || profiles.busy || profiles.remote.busy || reviewExport.busy || reanalysis.busy || previewBuffer.busy, dirty:scriptDocument.dirty || mindvisionDocument.dirty || configDirty || quickDraft.dirty || checkedConfig.dirty || profiles.dirty, report:(text)=>{append(text);setShowLog(true);}});
  return { requestClose };
}

type TelemetryDeps = Pick<PreviewState, "ratesRef"> & Pick<ConnectState, "running"> & Pick<ConfigState, "stats">;

// Display-side rates for the sidebar and the status bar.
export function shellTelemetry({ ratesRef, running, stats }: TelemetryDeps) {
  const r = ratesRef.current;
  const displayFps = running ? r.displayFps : 0;
  const dataRate = running ? r.dataRateMBs : 0;
  const algoFps = stats?.valid ? stats.algo_fps1s : null;
  const validFps = stats?.valid ? stats.valid_fps1s : null;
  const invalidFps = stats?.valid ? stats.invalid_fps1s : null;
  return { displayFps, dataRate, algoFps, validFps, invalidFps };
}
