import { FramePullScheduler } from "./framePullScheduler";
import { useCallback, useEffect, useRef, useState } from "react";
import { open, save } from "@tauri-apps/plugin-dialog";
import { openUrl, revealItemInDir } from "@tauri-apps/plugin-opener";
import {
  bridge,
  mono8ToImageData,
  type AutofocusStatus,
  type BridgeEvent,
  type CameraDiscovery,
  type CameraSelection,
  type ExperimentStatus,
  type FrameMeta,
  type FramePacket,
  type MonitoringSnapshot,
  type ProcessingCoreStatus,
  type ProcessingStats,
  type PumpStatus,
  type ReviewMetadata,
  type ReviewMetricsPage,
  type TriggerStatus,
} from "./bridge";
import { BRIDGE_ABI_VERSION, EXPERIMENT_STATES, PUMP_IDS } from "./bridgeContract";
import { deriveWorkflow, type StageTab, type WorkflowFacts } from "./workflow";
import { CHECK_STATUS_LABEL, derivePreflight, type PreflightInput } from "./preflight";
import { deriveQualityGates, GATE_STATUS_LABEL, type QualityInput } from "./quality";
import { deriveContextBar, SEG_STATUS_LABEL, type ContextBarFacts } from "./contextBar";
import {
  canActuate,
  canStartPeriodic,
  canStopPeriodic,
  DEFAULT_MODE,
  type OperatingMode,
} from "./commissioning";
import "./App.css";

const H5_FILTER = [{ name: "HDF5", extensions: ["h5"] }];
const SIDEBAR_KEY = "mib.sidebar.collapsed";

// Standard reason strings for controls whose backend surface is not bridged
// yet. Shown as tooltips — the control stays visible (Qt parity) but cannot
// be activated, and never fakes backend or hardware state.
const PENDING = {
  discovery: "Device discovery is not bridged yet — backend issue BE-2 (#272)",
  roi: "ROI editing is not bridged yet — backend issue BE-3 (#273)",
  script: "Camera script/config apply is not bridged yet — BE-2 (#272) / BE-3 (#273)",
  config: "App config / profiles are not bridged yet — backend issue BE-3 (#273)",
  profiles: "Profile management is not bridged yet — BE-3 (#273) follow-up",
  saveBuffer: "Preview buffer save is not bridged yet — UI-3 (#268)",
  monitoring: "Monitoring data is not bridged yet — backend issue BE-5 (#275)",
  review: "HDF5 metadata/metrics/export are not bridged yet — backend issue BE-6 (#276)",
  autofocus: "Autofocus/nanopositioner control is not bridged yet — BE-8 (#278)",
  platform: "Platform/shell services are not migrated yet — BE-9 (#279)",
  background: "Background image control is not bridged yet — backend issue BE-3 (#273)",
};

const EXPERIMENT_STATE_NAMES: Record<number, string> = {
  [EXPERIMENT_STATES.Idle]: "Inactive",
  [EXPERIMENT_STATES.Starting]: "Starting",
  [EXPERIMENT_STATES.Active]: "Active",
  [EXPERIMENT_STATES.Stopping]: "Stopping",
  [EXPERIMENT_STATES.Failed]: "Failed",
};

function formatRuntime(startNs: number, endNs: number): string {
  if (!startNs) return "00:00:00";
  const endMs = endNs > 0 ? endNs / 1e6 : Date.now();
  const totalS = Math.max(0, Math.floor((endMs - startNs / 1e6) / 1000));
  const h = String(Math.floor(totalS / 3600)).padStart(2, "0");
  const m = String(Math.floor((totalS % 3600) / 60)).padStart(2, "0");
  const s = String(totalS % 60).padStart(2, "0");
  return `${h}:${m}:${s}`;
}

type MainTab = "connect" | "overview" | "experiment" | "review";

interface RatesRef {
  windowStartMs: number;
  frames: number;
  bytes: number;
  displayFps: number;
  dataRateMBs: number;
  lastFrameIndex: string | null;
}

function SideRow(props: { k: string; v: string; cls?: string }) {
  return (
    <div className="side-row">
      <span className="k">{props.k}</span>
      <span className={`v ${props.cls ?? ""}`}>{props.v}</span>
    </div>
  );
}

// A menu-bar dropdown. Items with a `pending` reason render disabled with the
// reason as tooltip.
function Menu(props: {
  label: string;
  items: { label: string; onClick?: () => void; pending?: string }[];
}) {
  const [openMenu, setOpenMenu] = useState(false);
  return (
    <div className="menubar-item">
      <button aria-expanded={openMenu} onClick={() => setOpenMenu((o) => !o)} onBlur={() => window.setTimeout(() => setOpenMenu(false), 150)}>
        {props.label}
      </button>
      {openMenu && (
        <div className="menu-popup" role="menu">
          {props.items.map((it) => (
            <button
              key={it.label}
              role="menuitem"
              disabled={!!it.pending}
              title={it.pending}
              onClick={() => {
                setOpenMenu(false);
                it.onClick?.();
              }}
            >
              {it.label}
            </button>
          ))}
        </div>
      )}
    </div>
  );
}

// Operator shell aligned with the Qt application on main (issue #266):
// menu row, collapsible telemetry sidebar, Connect / Overview / Experiment /
// Review tabs with camera actions in the header, and a metrics status bar.
// All implemented bridge schema-v3 actions stay wired; everything else is
// visible but disabled with an explanatory tooltip.
export default function App() {
  const [abi, setAbi] = useState<number | null>(null);
  const [ready, setReady] = useState(false);
  const [running, setRunning] = useState(false);
  const [camStatus, setCamStatus] = useState("unconfigured");
  const [log, setLog] = useState<string[]>([]);
  const [showLog, setShowLog] = useState(false);
  const [lastMeta, setLastMeta] = useState<FrameMeta | null>(null);

  // Shell state.
  const [tab, setTab] = useState<MainTab>("connect");
  // UX-1 guided workflow: explicit operator confirmations, stored as the
  // device/core signature they were confirmed against so they auto-invalidate
  // when the hardware changes. Kept in memory (not persisted) so every session
  // re-confirms readiness. `didInitStage` guards the one-time startup landing.
  const [preflightConfirmedFor, setPreflightConfirmedFor] = useState("");
  const [alignmentConfirmedFor, setAlignmentConfirmedFor] = useState("");
  const didInitStage = useRef(false);
  const [connectTab, setConnectTab] = useState<"cameras" | "mindvision" | "framegrabbers">("cameras");
  const [expTab, setExpTab] = useState<"preview" | "monitoring">("preview");
  const [configTab, setConfigTab] = useState<"app" | "script">("app");
  const [sidebarCollapsed, setSidebarCollapsed] = useState(
    () => localStorage.getItem(SIDEBAR_KEY) === "1",
  );
  const [fitWindow, setFitWindow] = useState(true);
  const [showAbout, setShowAbout] = useState(false);

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

  // Recording.
  const [recording, setRecording] = useState(false);
  const [recPath, setRecPath] = useState("");

  // Processing (bridge schema v3).
  const [procEnabled, setProcEnabled] = useState(false);
  const [pixelToMicron, setPixelToMicron] = useState("1.0");
  const [stats, setStats] = useState<ProcessingStats | null>(null);

  // Experiment (bridge schema v5, BE-4 — backend-owned lifecycle).
  const [expStatus, setExpStatus] = useState<ExperimentStatus | null>(null);

  // Monitoring + trigger (bridge schema v6, BE-5).
  const [monSnapshot, setMonSnapshot] = useState<MonitoringSnapshot | null>(null);
  const [trigStatus, setTrigStatus] = useState<TriggerStatus | null>(null);
  const [pulseUs, setPulseUs] = useState("1");
  const [periodicMs, setPeriodicMs] = useState("1000");
  // UX-9: routine Operator vs Service/Commissioning mode. Every session starts
  // in Operator mode; `triggerArmed` is a one-shot safety arm for actuation.
  const [operatingMode, setOperatingMode] = useState<OperatingMode>(DEFAULT_MODE);
  const [triggerArmed, setTriggerArmed] = useState(false);

  // Autofocus status (schema v11, BE-8).
  const [afStatus, setAfStatus] = useState<AutofocusStatus | null>(null);
  // UX-3 preflight: pump/trigger snapshots polled while on the Preflight stage.
  const [samplePump, setSamplePump] = useState<PumpStatus | null>(null);
  const [sheathPump, setSheathPump] = useState<PumpStatus | null>(null);

  // Processing config / ROI / background / core identity (schema v8, BE-3).
  const [configText, setConfigText] = useState("");
  const [configDirty, setConfigDirty] = useState(false);
  const [coreStatus, setCoreStatus] = useState<ProcessingCoreStatus | null>(null);
  const [backgroundSet, setBackgroundSet] = useState(false);
  const [roiFields, setRoiFields] = useState({ x: "0", y: "0", w: "0", h: "0" });

  // Review.
  const [reviewPath, setReviewPath] = useState("");
  const [reviewing, setReviewing] = useState(false);
  const [reviewTab, setReviewTab] = useState<"raw" | "valid" | "invalid" | "charts">("raw");
  const [range, setRange] = useState({ earliest: 0, latest: 0, count: 0 });
  const [reviewIndex, setReviewIndex] = useState(0);
  // Paged review (bridge schema v9, BE-6).
  const [reviewMeta, setReviewMeta] = useState<ReviewMetadata | null>(null);
  const [metricsPage, setMetricsPage] = useState<ReviewMetricsPage | null>(null);
  const [metricsOffset, setMetricsOffset] = useState(0);
  const [reviewImgIndex, setReviewImgIndex] = useState(0);
  const METRICS_PAGE_SIZE = 50;

  const liveCanvasRef = useRef<HTMLCanvasElement>(null);
  const previewCanvasRef = useRef<HTMLCanvasElement>(null);
  const reviewCanvasRef = useRef<HTMLCanvasElement>(null);
  const loopRef = useRef<number | null>(null);
  const previewLoopRef = useRef<number | null>(null);
  const framePulls = useRef(new FramePullScheduler());
  const tickBusy = useRef(false);
  const lastMetadataRenderMs = useRef(-Infinity);
  const tabRef = useRef<MainTab>("connect");
  tabRef.current = tab;

  // Display-side measurements (frames actually drawn / bytes actually pulled
  // over the last 1s window). These are UI measurements, not backend claims.
  const ratesRef = useRef<RatesRef>({
    windowStartMs: performance.now(),
    frames: 0,
    bytes: 0,
    displayFps: 0,
    dataRateMBs: 0,
    lastFrameIndex: null,
  });
  const [, setRatesTick] = useState(0);

  const append = useCallback((line: string) => {
    setLog((l) => [`${new Date().toLocaleTimeString()} ${line}`, ...l].slice(0, 50));
    // Shell-side log sink (BE-9): every drawer line also lands in
    // <app_log>/desktop-shell.log for correlation with the backend logs.
    void bridge.shellLog("info", line).catch(() => {});
  }, []);

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
        const already = await bridge.isInitialized();
        const ok = already || (await bridge.init(""));
        setReady(ok);
        append(ok ? "backend initialized" : "backend init failed");
      } catch (e) {
        append(`init error: ${e}`);
      }
    })();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const activeLiveCanvas = useCallback((): HTMLCanvasElement | null => {
    if (tabRef.current === "experiment") return previewCanvasRef.current;
    return liveCanvasRef.current;
  }, []);

  // Draw only the pixels owned by this exact immutable pull response.
  const draw = useCallback(
    (meta: FramePacket, canvas: HTMLCanvasElement | null) => {
      if (!meta.valid || !canvas) return;
      const bytes = meta.data;
      canvas.width = meta.width;
      canvas.height = meta.height;
      const ctx = canvas.getContext("2d");
      if (!ctx) return;
      ctx.putImageData(mono8ToImageData(bytes, meta.width, meta.height, meta.stride_bytes), 0, 0);
      const { data: _pixels, ...metadata } = meta;
      if (canvas !== reviewCanvasRef.current && performance.now() - lastMetadataRenderMs.current >= 200) {
        lastMetadataRenderMs.current = performance.now();
        setLastMeta(metadata);
      }
      const r = ratesRef.current;
      r.frames += 1;
      r.bytes += meta.byte_len;
      r.lastFrameIndex = meta.frame_index;
      const now = performance.now();
      const elapsed = now - r.windowStartMs;
      if (elapsed >= 1000) {
        r.displayFps = (r.frames * 1000) / elapsed;
        r.dataRateMBs = (r.bytes * 1000) / elapsed / (1024 * 1024);
        r.frames = 0;
        r.bytes = 0;
        r.windowStartMs = now;
        setRatesTick((t) => t + 1);
      }
    },
    [],
  );

  const applyEvents = useCallback(
    (events: BridgeEvent[]) => {
      for (const e of events) {
        if (e.kind === "CameraStatus") {
          setCamStatus(`${e.b1 ? "running" : e.b0 ? "configured" : "unconfigured"} (${e.text || "camera"})`);
        } else if (e.kind === "PlaybackPosition") {
          // u2 earliest, u3 latest, u4 available.
          setRange({ earliest: e.u2, latest: e.u3, count: e.u4 });
        } else if (e.kind === "BackendError") {
          append(`backend error: ${e.text}`);
        } else if (e.kind === "OperationStatus") {
          // u0 id, u2 state (2 Completed, 3 Failed, 4 Cancelled, 5 TimedOut).
          if (e.u2 === 2) append(`operation ${e.u0} completed: ${e.text}`);
          else if (e.u2 >= 3) append(`operation ${e.u0} ${e.u2 === 3 ? "failed" : e.u2 === 4 ? "cancelled" : "timed out"}: ${e.text}`);
        } else if (e.kind === "QueueOverflow") {
          append(`event queue overflow: ${e.u0} coalesced (total ${e.u1})`);
        } else if (e.kind === "ExperimentStatus") {
          // Transitions surface in the log; the full snapshot is pulled in the
          // tick loop (fetch_experiment_status) to keep one source of truth.
          if (e.u0 === EXPERIMENT_STATES.Failed) append(`experiment failed: ${e.text}`);
          else if (e.text) append(`experiment: ${e.text}`);
          void bridge.fetchExperimentStatus().then(setExpStatus).catch(() => {});
        }
        // Kinds this build does not know (additive, newer bridge) fall through
        // and are ignored — never fatal (ADR 0004).
      }
    },
    [append],
  );

  const procEnabledRef = useRef(procEnabled);
  procEnabledRef.current = procEnabled;

  useEffect(() => {
    const scheduler = framePulls.current;
    const live = scheduler.mount("live", p => draw(p, activeLiveCanvas()), e => append(`frame error: ${e}`));
    const review = scheduler.mount("review", p => draw(p, reviewCanvasRef.current), e => append(`review frame error: ${e}`));
    return () => { live(); review(); };
  }, [draw, activeLiveCanvas, append]);

  // Navigation/config/source changes retire presentation replies only.
  useEffect(() => { framePulls.current.invalidate(); }, [tab, reviewPath, camStatus]);

  const tick = useCallback(async () => {
    if (tickBusy.current) return;
    tickBusy.current = true;
    try {
      // State/fault notifications do not wait behind pixel decode/rendering.
      applyEvents(await bridge.pollEvents());
      if (procEnabledRef.current) setStats(await bridge.fetchProcessingStats());
      setExpStatus(await bridge.fetchExperimentStatus());
      setAfStatus(await bridge.fetchAutofocusStatus());
    } catch (e) {
      append(`tick error: ${e}`);
    } finally { tickBusy.current = false; }
  }, [applyEvents, append]);

  const stopLoop = useCallback(() => {
    framePulls.current.invalidate("live");
    if (previewLoopRef.current !== null) {
      window.clearInterval(previewLoopRef.current);
      previewLoopRef.current = null;
    }
    if (loopRef.current !== null) {
      window.clearInterval(loopRef.current);
      loopRef.current = null;
    }
  }, []);

  useEffect(() => () => stopLoop(), [stopLoop]);

  // Monitoring is visibility-gated (BE-5): accumulation and its per-frame
  // image clones run only while the Monitoring view is actually shown.
  const monitoringVisible = tab === "experiment" && expTab === "monitoring";
  useEffect(() => {
    if (!ready) return;
    bridge.monitoringSetActive(monitoringVisible).catch(() => {});
    if (!monitoringVisible) return;
    const id = window.setInterval(async () => {
      try {
        setMonSnapshot(await bridge.fetchMonitoringSnapshot(200));
        setTrigStatus(await bridge.fetchTriggerStatus());
      } catch {
        /* backend gone — next tick will surface it */
      }
    }, 500);
    return () => window.clearInterval(id);
  }, [monitoringVisible, ready]);

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

  // ---- Camera discovery/selection (BE-2) + camera actions ----

  const refreshConfig = useCallback(async () => {
    try {
      const doc = await bridge.fetchProcessingConfigJson();
      if (doc.valid) {
        const parsed = JSON.parse(doc.json);
        setConfigText(JSON.stringify(parsed, null, 2));
        setConfigDirty(false);
        setBackgroundSet(Boolean(parsed.background_set));
        if (parsed.roi) {
          setRoiFields({
            x: String(parsed.roi.x ?? 0),
            y: String(parsed.roi.y ?? 0),
            w: String(parsed.roi.w ?? 0),
            h: String(parsed.roi.h ?? 0),
          });
        }
      }
      setCoreStatus(await bridge.fetchProcessingCoreStatus());
    } catch (e) {
      append(`config fetch error: ${e}`);
    }
  }, [append]);

  useEffect(() => {
    if (ready) void refreshConfig();
  }, [ready, refreshConfig]);

  const onApplyConfigJson = useCallback(async () => {
    try {
      const res = await bridge.applyProcessingConfigJson(configText);
      if (!res.ok) return append(`config apply failed: ${res.message}`);
      append("processing config applied");
      await refreshConfig();
    } catch (e) {
      append(`config apply error: ${e}`);
    }
  }, [configText, append, refreshConfig]);

  const onApplyRoi = useCallback(async () => {
    try {
      const res = await bridge.setProcessingRoi(
        Number(roiFields.x) || 0,
        Number(roiFields.y) || 0,
        Number(roiFields.w) || 0,
        Number(roiFields.h) || 0,
      );
      if (!res.ok) return append(`ROI apply failed: ${res.message}`);
      append(`ROI set to ${roiFields.w}×${roiFields.h} @ (${roiFields.x}, ${roiFields.y})`);
      await refreshConfig();
    } catch (e) {
      append(`ROI error: ${e}`);
    }
  }, [roiFields, append, refreshConfig]);

  const refreshCameraState = useCallback(async () => {
    try {
      setDiscovery(await bridge.fetchCameraDiscovery());
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
      append("capture started");
      stopLoop();
      loopRef.current = window.setInterval(tick, 200);
      previewLoopRef.current = window.setInterval(() => {
        framePulls.current.request("live", bridge.fetchFrame, false);
      }, 1000 / 30);
      // Parity with Qt: a successful start lands the operator on Overview.
      setTab((t) => (t === "connect" ? "overview" : t));
    } catch (e) {
      append(`start error: ${e}`);
    }
  }, [tick, append, stopLoop]);

  const onStopCamera = useCallback(async () => {
    stopLoop();
    try {
      if (recording) {
        await bridge.stopRecording();
        setRecording(false);
      }
      await bridge.stopCapture();
      setRunning(false);
      append("capture stopped");
    } catch (e) {
      append(`stop error: ${e}`);
    }
  }, [recording, stopLoop, append]);

  // ---- Recording (Experiment ▸ Preview toolbar) ----

  const onToggleRecord = useCallback(async () => {
    try {
      if (!recording) {
        const picked = await save({ title: "Recording output", filters: H5_FILTER, defaultPath: recPath || "clip.h5" });
        if (!picked) return;
        setRecPath(picked);
        const res = await bridge.startRecording(picked);
        if (!res.ok) return append(`record failed: ${res.message}`);
        setRecording(true);
        append(`recording → ${picked}`);
      } else {
        await bridge.stopRecording();
        setRecording(false);
        append("recording stopped");
      }
    } catch (e) {
      append(`record error: ${e}`);
    }
  }, [recording, recPath, append]);

  // ---- Processing settings (bridged subset of App config) ----

  const onApplyProcessing = useCallback(async () => {
    try {
      const factor = Number(pixelToMicron) || 1.0;
      const res = await bridge.applyProcessing(procEnabled, factor);
      if (!res.ok) return append(`processing failed: ${res.message}`);
      append(`processing ${procEnabled ? "enabled" : "disabled"} (px→µm ${factor})`);
      setStats(await bridge.fetchProcessingStats());
    } catch (e) {
      append(`processing error: ${e}`);
    }
  }, [procEnabled, pixelToMicron, append]);

  // ---- Experiment lifecycle (backend-owned, BE-4) ----

  const onStartExperiment = useCallback(async () => {
    try {
      const picked = await save({ title: "Save Experiment Data", filters: H5_FILTER, defaultPath: "experiment.h5" });
      if (!picked) return;
      const res = await bridge.experimentStart(picked);
      if (!res.ok) return append(`experiment start failed: ${res.message}`);
      append(`experiment started → ${picked}`);
      setExpStatus(await bridge.fetchExperimentStatus());
    } catch (e) {
      append(`experiment start error: ${e}`);
    }
  }, [append]);

  const onStopExperiment = useCallback(async () => {
    try {
      const res = await bridge.experimentStop();
      if (!res.ok) return append(`experiment stop failed: ${res.message}`);
      setExpStatus(await bridge.fetchExperimentStatus());
    } catch (e) {
      append(`experiment stop error: ${e}`);
    }
  }, [append]);

  // ---- Review ----

  const onScrub = useCallback((idx: number) => {
    setReviewIndex(idx);
    framePulls.current.request("review", async () => {
      const result = await bridge.seekIndex(idx);
      if (!result.ok) throw new Error(result.message);
      return bridge.fetchFrameByIndex(idx);
    });
  }, []);

  const loadMetricsPage = useCallback(
    async (valid: boolean, offset: number) => {
      try {
        const page = await bridge.fetchReviewMetricsPage(valid, offset, METRICS_PAGE_SIZE);
        if (page.valid) {
          setMetricsPage(page);
          setMetricsOffset(offset);
        }
      } catch (e) {
        append(`metrics page error: ${e}`);
      }
    },
    [append],
  );

  const drawReviewImage = useCallback((dataset: number, index: number) => {
    framePulls.current.request("review", () => bridge.fetchReviewImage(dataset, index));
  }, []);

  const onSelectHdf = useCallback(async () => {
    const picked = await open({ title: "Open recording", filters: H5_FILTER, multiple: false });
    if (typeof picked !== "string") return;
    stopLoop();
    setRunning(false);
    setReviewPath(picked);
    try {
      const res = await bridge.loadRecording(picked);
      if (!res.ok) return append(`load failed: ${res.message}`);
      setReviewing(true);
      append(`loaded ${picked}`);
      applyEvents(await bridge.pollEvents());
      const meta = await bridge.fetchReviewMetadata();
      setReviewMeta(meta);
      setReviewTab(meta.recording_file ? "raw" : "valid");
      setReviewImgIndex(0);
      await loadMetricsPage(true, 0);
      if (meta.recording_file) {
        await onScrub(range.earliest);
      } else if (meta.valid_images.present && meta.valid_images.count > 0) {
        await drawReviewImage(0, 0);
      }
    } catch (e) {
      append(`load error: ${e}`);
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [append, applyEvents, range.earliest, stopLoop, onScrub, loadMetricsPage, drawReviewImage]);

  const onExportCsv = useCallback(async () => {
    try {
      const picked = await save({
        title: "Export Metrics to CSV",
        filters: [{ name: "CSV", extensions: ["csv"] }],
        defaultPath: "metrics.csv",
      });
      if (!picked) return;
      const res = await bridge.reviewExportCsv(picked);
      append(res.ok ? `CSV export started (operation ${res.operation_id})` : `export failed: ${res.message}`);
    } catch (e) {
      append(`export error: ${e}`);
    }
  }, [append]);

  const openReviewFromMenu = useCallback(() => {
    setTab("review");
    void onSelectHdf();
  }, [onSelectHdf]);

  const r = ratesRef.current;
  const displayFps = running ? r.displayFps : 0;
  const dataRate = running ? r.dataRateMBs : 0;
  const algoFps = stats?.valid ? stats.algo_fps1s : 0;
  const validFps = stats?.valid ? stats.valid_fps1s : 0;
  const invalidFps = stats?.valid ? stats.invalid_fps1s : 0;

  const expState = expStatus?.valid ? expStatus.state : EXPERIMENT_STATES.Idle;
  const expActive = expState === EXPERIMENT_STATES.Active || expState === EXPERIMENT_STATES.Stopping;
  const startExperimentReason = !ready
    ? "Backend is not initialized"
    : !running
      ? "Camera must be running before starting an experiment"
      : expActive
        ? "Experiment is already running"
        : undefined;

  const cameraConfigured = camSelection?.configured ?? false;
  const startCameraReason = !ready
    ? "Backend is not initialized"
    : !cameraConfigured
      ? "No camera configured — select a device in the Connect tab"
      : running
        ? "Camera is already running"
        : undefined;

  // ---- UX-1 guided four-stage workflow (issue #305) ----
  // Signatures the operator confirms against: change the device or the pinned
  // processing core and the prior confirmation no longer matches.
  const deviceIdentity = cameraConfigured
    ? `${camSelection?.mode ?? ""}|${camSelection?.label ?? ""}`
    : "";
  const preflightSignature = cameraConfigured
    ? `${deviceIdentity}|core:${coreStatus?.valid ? coreStatus.active_version : ""}`
    : "";
  const alignmentSignature = deviceIdentity;
  const experimentCompleted =
    expState === EXPERIMENT_STATES.Idle &&
    !!expStatus?.valid &&
    !expStatus.cancelled &&
    expStatus.output_path !== "" &&
    expStatus.valid_saved + expStatus.invalid_saved > 0;

  const workflowFacts: WorkflowFacts = {
    backendReady: ready,
    cameraConfigured,
    cameraRunning: running,
    preflightSignature,
    preflightConfirmedFor,
    alignmentSignature,
    alignmentConfirmedFor,
    coreValid: coreStatus?.valid ?? false,
    corePinSatisfied: coreStatus?.pin_satisfied ?? false,
    requiredCoreVersion: coreStatus?.required_version ?? "",
    experimentState: expState,
    experimentCompleted,
    reviewFileOpen: reviewMeta?.file_open ?? false,
    reviewValid: reviewMeta?.valid ?? false,
  };
  const workflow = deriveWorkflow(workflowFacts);
  const stageByTab = Object.fromEntries(workflow.stages.map((s) => [s.tab, s])) as Record<
    StageTab,
    (typeof workflow.stages)[number]
  >;
  const currentStage = workflow.stages.find((s) => s.id === workflow.currentStageId)!;
  // A confirmation cannot be applied while a run is active (setup is locked).
  const recNeedsConfirm = !!workflow.recommended && workflow.recommended.kind !== "navigate";
  const recDisabled = recNeedsConfirm && expActive;

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
      identity: afStatus?.valid ? `COM${afStatus.com_port}` : "",
    },
    samplePump: {
      valid: samplePump?.valid ?? false,
      connected: samplePump?.connected ?? false,
      identity: samplePump?.valid ? `COM${samplePump.com_port}` : "",
    },
    sheathPump: {
      valid: sheathPump?.valid ?? false,
      connected: sheathPump?.connected ?? false,
      identity: sheathPump?.valid ? `COM${sheathPump.com_port}` : "",
    },
    trigger: { valid: trigStatus?.valid ?? false, cameraAttached: trigStatus?.camera_attached ?? false },
    // Authoritative storage/free-space status is not bridged yet (backend
    // follow-up); the check stays informational until it is.
    storageKnown: false,
    storageWritable: false,
    storageFreeOk: false,
    storagePath: "",
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
    pixelToMicron: stats?.valid ? stats.pixel_to_micron : Number(pixelToMicron) || 0,
  };
  const quality = deriveQualityGates(qualityInput);

  // ---- UX-8 persistent active-context bar (issue #312) ----
  // Warnings = unresolved attention items surfaced by preflight + quality.
  const warningsCount = preflight.failed + preflight.warning + quality.warn + quality.fail;
  const contextFacts: ContextBarFacts = {
    profileName: "", // Experiment Profile management not bridged yet (UX-2 #306)
    cameraConfigured,
    cameraRunning: running,
    cameraLabel: camSelection?.label || (camSelection?.mode === 1 ? "Mock camera" : ""),
    pixelToMicron: stats?.valid ? stats.pixel_to_micron : Number(pixelToMicron) || 0,
    currentStageTitle: currentStage.title,
    currentStageStatus: currentStage.status,
    allStagesComplete: workflow.recommended === null,
    experimentActive: expActive,
    experimentFailed: expState === EXPERIMENT_STATES.Failed,
    operatorName: "", // operator identity not captured yet
    outputPath: expStatus?.output_path ?? "",
    warningsCount,
  };
  const contextBar = deriveContextBar(contextFacts);

  // ---- UX-9 Operator vs Service/Commissioning mode (issue #313) ----
  // Leaving Service mode, or an experiment going active, disarms actuation.
  useEffect(() => {
    if (operatingMode !== "service" || expActive) setTriggerArmed(false);
  }, [operatingMode, expActive]);

  const enterMode = (next: OperatingMode) => {
    if (next === "service") {
      const ok = window.confirm(
        "Enter Service / Commissioning mode?\n\nThis exposes hardware-actuating controls (trigger tests). Use only for bring-up and diagnostics.",
      );
      if (!ok) return;
      append("entered Service / Commissioning mode");
    } else {
      append("returned to Operator mode");
    }
    setOperatingMode(next);
    if (next !== "service") setTriggerArmed(false);
  };

  const actuateCheck = canActuate({
    mode: operatingMode,
    experimentActive: expActive,
    triggerAttached: trigStatus?.camera_attached ?? false,
    armed: triggerArmed,
  });
  const startPeriodicCheck = canStartPeriodic({
    mode: operatingMode,
    experimentActive: expActive,
    triggerAttached: trigStatus?.camera_attached ?? false,
    armed: triggerArmed,
  });
  const stopPeriodicCheck = canStopPeriodic({ periodicActive: trigStatus?.periodic_active ?? false });

  const doRecommended = () => {
    const rec = workflow.recommended;
    if (!rec) return;
    if (rec.kind === "confirm-preflight" && !expActive) {
      setPreflightConfirmedFor(preflightSignature);
      setTab("connect");
    } else if (rec.kind === "confirm-alignment" && !expActive) {
      setAlignmentConfirmedFor(alignmentSignature);
      setTab("overview");
    } else {
      setTab(rec.tab);
    }
  };

  // Startup lands on the earliest incomplete stage (UX-1), unless an active or
  // recoverable experiment demands the Experiment stage. Runs once, after the
  // first camera snapshot arrives so the landing reflects real backend state.
  useEffect(() => {
    if (didInitStage.current || !ready || camSelection === null) return;
    didInitStage.current = true;
    if (expActive || expState === EXPERIMENT_STATES.Failed) {
      setTab("experiment");
      return;
    }
    const cur = workflow.stages.find((s) => s.id === workflow.currentStageId);
    if (cur) setTab(cur.tab);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [ready, camSelection, expActive, expState]);

  return (
    <div className="app">
      {/* ---- Menu row ---- */}
      <nav className="menubar" aria-label="Main menu">
        <Menu
          label="File"
          items={[
            { label: "Open Recording…", onClick: openReviewFromMenu },
            {
              label: "Open Data Folder",
              onClick: () => {
                void bridge
                  .appPaths()
                  .then((paths) => revealItemInDir(paths.app_data))
                  .catch((e) => append(`open data folder failed: ${e}`));
              },
            },
            { label: "Exit", pending: PENDING.platform },
          ]}
        />
        <Menu
          label="Settings"
          items={[
            { label: "Processing Settings…", pending: PENDING.config },
            { label: "Pixel to Micron…", pending: PENDING.config },
            { label: "Monitoring Settings…", pending: PENDING.monitoring },
            { label: "Updates…", pending: PENDING.platform },
          ]}
        />
        <Menu
          label="Help"
          items={[
            { label: "About", onClick: () => setShowAbout(true) },
            {
              label: "Documentation",
              onClick: () => {
                void openUrl("https://kpt1020.github.io/mib-studio-qt/").catch((e) =>
                  append(`open documentation failed: ${e}`),
                );
              },
            },
            { label: "Report a Problem…", pending: PENDING.platform },
          ]}
        />
        <div className="menubar-spacer" />
        <button
          type="button"
          className={`mode-toggle ${operatingMode}`}
          onClick={() => enterMode(operatingMode === "service" ? "operator" : "service")}
          title="Switch between routine Operator mode and Service / Commissioning mode"
          aria-label={`Operating mode: ${operatingMode === "service" ? "Service / Commissioning" : "Operator"}. Click to switch.`}
        >
          Mode: {operatingMode === "service" ? "Service ⚠" : "Operator"}
        </button>
      </nav>

      {operatingMode === "service" && (
        <div className="service-banner" role="alert">
          <span>
            <strong>Service / Commissioning mode</strong> — hardware-actuating controls are enabled.
            Not for routine operation.
          </span>
          <button type="button" onClick={() => enterMode("operator")}>
            Exit to Operator
          </button>
        </div>
      )}

      <div className="body">
        {/* ---- Telemetry sidebar ---- */}
        <aside className={`sidebar ${sidebarCollapsed ? "collapsed" : ""}`} aria-label="Telemetry sidebar">
          <div className="side-section">
            <div className="bg-preview" title="Processing background state (set/clear in Experiment ▸ Preview)">
              {backgroundSet ? "Background set" : "No background set"}
            </div>
          </div>
          <div className="side-section">
            <h4>Display</h4>
            <SideRow k="FPS:" v={displayFps.toFixed(1)} />
          </div>
          <div className="side-section">
            <h4>Processing</h4>
            <SideRow k="Algo FPS:" v={stats?.valid ? algoFps.toFixed(1) : "—"} cls={stats?.valid ? "" : "dim"} />
            <SideRow k="Valid FPS:" v={stats?.valid ? validFps.toFixed(1) : "—"} cls={stats?.valid ? "" : "dim"} />
            <SideRow k="Invalid FPS:" v={stats?.valid ? invalidFps.toFixed(1) : "—"} cls={stats?.valid ? "" : "dim"} />
            <SideRow k="px→µm:" v={stats?.valid ? String(stats.pixel_to_micron) : "—"} cls={stats?.valid ? "" : "dim"} />
          </div>
          <div className="side-section">
            <h4>Camera</h4>
            <SideRow k="Status:" v={running ? "Running" : camStatus} cls={running ? "ok" : ""} />
            <SideRow k="Display rate:" v={`${displayFps.toFixed(1)} fps`} />
            <SideRow k="Data rate:" v={`${dataRate.toFixed(1)} MB/s`} />
          </div>
          <div className="side-section">
            <h4>Autofocus</h4>
            <SideRow
              k="Ring width:"
              v={afStatus?.valid && afStatus.last_ring_ratio_update_us > 0 ? afStatus.median_ring_ratio.toFixed(3) : "—"}
              cls={afStatus?.valid && afStatus.last_ring_ratio_update_us > 0 ? "" : "dim"}
            />
            <SideRow
              k="Controller:"
              v={afStatus?.connected ? `connected (${afStatus.enabled ? "auto" : "manual"})` : "disconnected"}
              cls={afStatus?.connected ? "ok" : "dim"}
            />
          </div>
          <div className="side-section">
            <h4>Experiment</h4>
            <SideRow
              k="Status:"
              v={EXPERIMENT_STATE_NAMES[expState] ?? "Inactive"}
              cls={expActive ? "ok" : expState === EXPERIMENT_STATES.Failed ? "" : "dim"}
            />
            <SideRow k="Valid Buffered:" v={String(expStatus?.valid_buffered ?? 0)} />
            <SideRow k="Invalid Buffered:" v={String(expStatus?.invalid_buffered ?? 0)} />
            <SideRow k="Flush Status:" v={expStatus?.flushing ? "Flushing" : "Idle"} />
            <SideRow k="Valid Images Saved:" v={String(expStatus?.valid_saved ?? 0)} />
            <SideRow
              k="Runtime:"
              v={expActive ? formatRuntime(expStatus?.start_time_ns ?? 0, 0) : "00:00:00"}
            />
          </div>
          <div className="side-section" title="Nanopositioner control panel lands with UI-3 (#268); values are the live backend state">
            <h4>Nanopositioner Autofocus</h4>
            <SideRow
              k="COM Port:"
              v={afStatus?.valid ? `COM${afStatus.com_port}` : "—"}
              cls={afStatus?.valid ? "" : "dim"}
            />
            <SideRow
              k="Voltage:"
              v={afStatus?.connected ? `${afStatus.current_voltage.toFixed(1)} V` : "—"}
              cls={afStatus?.connected ? "" : "dim"}
            />
            <SideRow
              k="Metric age:"
              v={
                afStatus?.valid && afStatus.last_ring_ratio_update_us > 0
                  ? `${(afStatus.ring_ratio_age_us / 1000).toFixed(0)} ms`
                  : "—"
              }
              cls={afStatus?.valid && afStatus.last_ring_ratio_update_us > 0 ? "" : "dim"}
            />
          </div>
        </aside>
        <button
          className="sidebar-toggle"
          onClick={toggleSidebar}
          title={sidebarCollapsed ? "Show telemetry sidebar" : "Hide telemetry sidebar"}
          aria-label={sidebarCollapsed ? "Show telemetry sidebar" : "Hide telemetry sidebar"}
        >
          {sidebarCollapsed ? "▶" : "◀"}
        </button>

        {/* ---- Main tabbed area ---- */}
        <main className="main">
          <div className="tabs-header">
            <div className="tabbar" role="tablist" aria-label="Workflow stages">
              {(["connect", "overview", "experiment", "review"] as StageTab[]).map((t) => {
                const stage = stageByTab[t];
                return (
                  <button
                    key={t}
                    role="tab"
                    aria-selected={tab === t}
                    aria-label={`${stage.title}: ${stage.statusLabel}`}
                    className={`stage-tab${tab === t ? " active" : ""}`}
                    title={stage.blocking.length ? stage.blocking.join("; ") : stage.summary}
                    onClick={() => setTab(t)}
                  >
                    <span className={`stage-dot ${stage.status}`} aria-hidden="true" />
                    <span className="stage-tab-text">
                      <span className="stage-tab-name">{stage.title}</span>
                      <span className="stage-tab-status">{stage.statusLabel}</span>
                    </span>
                  </button>
                );
              })}
            </div>
            <div className="spacer" />
            <div className="camera-actions">
              <button onClick={onStartCamera} disabled={!!startCameraReason} title={startCameraReason}>
                Start Camera
              </button>
              <button
                onClick={onStopCamera}
                disabled={!running || expActive}
                title={
                  !running
                    ? "Camera is not running"
                    : expActive
                      ? "Cannot stop camera while experiment is active. Please stop the experiment first."
                      : undefined
                }
              >
                Stop Camera
              </button>
            </div>
          </div>

          {ready && (
            <div className="workflow-next" role="status" aria-live="polite">
              {workflow.recommended ? (
                <>
                  <span className="workflow-next-text">
                    <strong>Next:</strong> {currentStage.title} — {currentStage.summary}
                  </span>
                  <button
                    className="workflow-next-action"
                    onClick={doRecommended}
                    disabled={recDisabled}
                    title={
                      recDisabled
                        ? "Setup is locked while an experiment is active"
                        : undefined
                    }
                  >
                    {workflow.recommended.label}
                  </button>
                </>
              ) : (
                <span className="workflow-next-text">
                  <strong>Workflow complete</strong> — all stages ready and data reviewed.
                </span>
              )}
            </div>
          )}

          <div className="tab-body">
            {/* ---- Connect ---- */}
            {tab === "connect" && (
              <>
                <p style={{ margin: "0 0 6px" }}>Available devices:</p>
                <div className="subtabs" role="tablist" aria-label="Device sources">
                  <button className={connectTab === "cameras" ? "active" : ""} onClick={() => setConnectTab("cameras")}>
                    Cameras
                  </button>
                  <button className={connectTab === "mindvision" ? "active" : ""} onClick={() => setConnectTab("mindvision")}>
                    MindVision
                  </button>
                  <button className={connectTab === "framegrabbers" ? "active" : ""} onClick={() => setConnectTab("framegrabbers")}>
                    Framegrabbers
                  </button>
                </div>
                <div className="subtab-body">
                  <div className="devices-list" role="listbox" aria-label="Discovered devices">
                    {connectTab !== "framegrabbers" &&
                      (discovery?.cameras ?? [])
                        .filter((c) => (connectTab === "mindvision" ? c.camera_type === 1 : c.camera_type !== 1))
                        .map((c) => {
                          const key = `${c.camera_type}:${c.interface_index}:${c.device_index}:${c.camera_index}`;
                          return (
                            <div key={key} style={{ padding: "2px 0" }}>
                              <label>
                                <input
                                  type="radio"
                                  name="device"
                                  checked={pickedDevice === key}
                                  onChange={() => setPickedDevice(key)}
                                />{" "}
                                {c.label}
                              </label>
                            </div>
                          );
                        })}
                    {connectTab === "framegrabbers" &&
                      (discovery?.framegrabbers ?? []).map((g) => (
                        <div key={`${g.interface_index}:${g.device_index}:${g.stream_index}`} style={{ padding: "2px 0" }}>
                          {g.label}
                        </div>
                      ))}
                    {connectTab === "mindvision" && (discovery?.cameras ?? []).every((c) => c.camera_type !== 1) && (
                      <div>No MindVision cameras found (SDK/hardware required).</div>
                    )}
                    {connectTab === "framegrabbers" && (discovery?.framegrabbers ?? []).length === 0 && (
                      <div>No framegrabbers found (EGrabber SDK/hardware required).</div>
                    )}
                  </div>
                  <div className="toolbar" style={{ marginTop: 8 }}>
                    <button onClick={refreshCameraState} disabled={!ready}>Refresh</button>
                    <button
                      onClick={onConnectPicked}
                      disabled={!ready || !pickedDevice}
                      title={pickedDevice ? undefined : "Pick a device first"}
                    >
                      Connect
                    </button>
                    <div className="right">
                      <button className="btn" onClick={() => setShowMockConfig(true)} disabled={!ready} title={ready ? undefined : "Backend is not initialized"}>
                        Configure Mock…
                      </button>
                    </div>
                  </div>
                  <p className="path-label">
                    Found {(discovery?.framegrabbers ?? []).length} framegrabber(s),{" "}
                    {(discovery?.cameras ?? []).filter((c) => c.camera_type === 0).length} eGrabber camera(s),{" "}
                    {(discovery?.cameras ?? []).filter((c) => c.camera_type === 1).length} MindVision camera(s)
                    {camSelection?.configured
                      ? ` · selected: ${
                          camSelection.mode === 1
                            ? `mock (${camSelection.mock_frame_dir || "no folder"})`
                            : camSelection.label || "camera"
                        }`
                      : " · no camera selected"}
                  </p>
                </div>

                {/* ---- UX-3 hardware preflight checklist ---- */}
                <div className="preflight-panel">
                  <div className="preflight-head">
                    <h4>Hardware preflight</h4>
                    <span className="preflight-summary">
                      {preflight.passed} passed · {preflight.warning} warning · {preflight.failed} failed
                      {preflight.notRequired ? ` · ${preflight.notRequired} n/a` : ""}
                    </span>
                    <button className="btn" onClick={() => void refreshPreflight()} disabled={!ready}>
                      Refresh
                    </button>
                  </div>
                  <ul className="preflight-list">
                    {preflight.checks.map((c) => (
                      <li key={c.id} className={`preflight-check ${c.status}`}>
                        <span className={`check-dot ${c.status}`} aria-hidden="true" />
                        <span className="check-main">
                          <span className="check-label">
                            {c.label}
                            {c.requirement === "required" && <span className="req-badge">required</span>}
                          </span>
                          <span className="check-detail">{c.detail}</span>
                          {(c.expected || c.detected !== "—") && (
                            <span className="check-identity">
                              {c.expected ? `expected ${c.expected} · ` : ""}
                              detected {c.detected}
                            </span>
                          )}
                        </span>
                        <span
                          className="check-status"
                          aria-label={`${c.label}: ${CHECK_STATUS_LABEL[c.status]}`}
                        >
                          {CHECK_STATUS_LABEL[c.status]}
                        </span>
                        {c.recovery.length > 0 && (
                          <span className="check-recovery">
                            {c.recovery.map((a) => (
                              <button
                                key={a.kind}
                                className="btn small"
                                onClick={() => void refreshPreflight()}
                                disabled={!ready}
                              >
                                {a.label}
                              </button>
                            ))}
                          </span>
                        )}
                      </li>
                    ))}
                  </ul>
                  <p className={`preflight-foot ${preflight.criticalPassed ? "ok" : "warn"}`}>
                    {preflight.criticalPassed
                      ? "Required checks pass — confirm readiness in the workflow bar to complete Preflight."
                      : "Required checks must pass before Preflight can be confirmed."}
                  </p>
                </div>
              </>
            )}

            {/* ---- Overview ---- */}
            {tab === "overview" && (
              <>
                <div className="toolbar">
                  <button onClick={() => setFitWindow((f) => !f)}>{fitWindow ? "Fit: Window" : "Fit: 1:1"}</button>
                  <button disabled title="ROI overlay rendering lands with UI-2 (#267)">ROI Overlay: Off</button>
                  <label>
                    X: <input type="number" value={roiFields.x} onChange={(e) => setRoiFields((r) => ({ ...r, x: e.target.value }))} />
                  </label>
                  <label>
                    Y: <input type="number" value={roiFields.y} onChange={(e) => setRoiFields((r) => ({ ...r, y: e.target.value }))} />
                  </label>
                  <label>
                    W: <input type="number" value={roiFields.w} onChange={(e) => setRoiFields((r) => ({ ...r, w: e.target.value }))} /> px
                  </label>
                  <label>
                    H: <input type="number" value={roiFields.h} onChange={(e) => setRoiFields((r) => ({ ...r, h: e.target.value }))} /> px
                  </label>
                  <button className="btn" onClick={onApplyRoi} disabled={!ready}>
                    Apply ROI
                  </button>
                </div>
                <div className="canvas-wrap">
                  {!lastMeta && <span className="canvas-hint">No frame yet — configure a camera and press Start Camera</span>}
                  <canvas ref={liveCanvasRef} className={fitWindow ? "fit" : ""} />
                </div>

                {/* ---- UX-4 image quality gates ---- */}
                <div className="quality-panel" aria-label="Image quality gates">
                  <div className="quality-head">
                    <h4>Quality gates</h4>
                    <span className="quality-summary">
                      {quality.pass} pass · {quality.warn} warn · {quality.fail} fail
                      {quality.unknown ? ` · ${quality.unknown} unknown` : ""}
                    </span>
                  </div>
                  <div className="quality-gates">
                    {quality.gates.map((g) => (
                      <div key={g.id} className={`quality-gate ${g.status}`} title={g.detail}>
                        <span className="gate-top">
                          <span className={`gate-dot ${g.status}`} aria-hidden="true" />
                          <span className="gate-label">{g.label}</span>
                        </span>
                        <span className="gate-value">{g.value}</span>
                        <span
                          className="gate-status"
                          aria-label={`${g.label}: ${GATE_STATUS_LABEL[g.status]}`}
                        >
                          {GATE_STATUS_LABEL[g.status]}
                        </span>
                      </div>
                    ))}
                  </div>
                </div>

                <div className="toolbar" style={{ marginTop: 6 }}>
                  <button disabled title={PENDING.script}>Reset</button>
                  <button disabled title={PENDING.script}>Save</button>
                  <button disabled title={PENDING.script}>Apply to Camera</button>
                  <button disabled title={PENDING.script}>Browse…</button>
                  <button disabled title={PENDING.script}>Clear</button>
                  <span className="path-label right">camera script: not bridged (BE-2 #272)</span>
                </div>
                <textarea
                  className="script-editor"
                  disabled
                  title={PENDING.script}
                  value={"// Camera script editing is not bridged yet — BE-2 (#272)."}
                  readOnly
                />
                <p className="mono">
                  {lastMeta
                    ? `#${lastMeta.frame_index} ${lastMeta.width}×${lastMeta.height} stride=${lastMeta.stride_bytes} bytes=${lastMeta.byte_len}`
                    : "no frame yet"}
                </p>
              </>
            )}

            {/* ---- Experiment ---- */}
            {tab === "experiment" && (
              <>
                <div className="toolbar">
                  <div className="subtabs" role="tablist" aria-label="Experiment views">
                    <button className={expTab === "preview" ? "active" : ""} onClick={() => setExpTab("preview")}>
                      Preview
                    </button>
                    <button className={expTab === "monitoring" ? "active" : ""} onClick={() => setExpTab("monitoring")}>
                      Monitoring
                    </button>
                  </div>
                  <div className="right">
                    <span className="mono">
                      ROI: {Number(roiFields.w) > 0 ? `${roiFields.w} × ${roiFields.h} @ (${roiFields.x}, ${roiFields.y})` : "full frame"}
                    </span>
                    <button onClick={onStartExperiment} disabled={!!startExperimentReason} title={startExperimentReason}>
                      Start Experiment
                    </button>
                    <button
                      onClick={onStopExperiment}
                      disabled={expState !== EXPERIMENT_STATES.Active}
                      title={expState === EXPERIMENT_STATES.Active ? undefined : "No experiment is currently running"}
                    >
                      Stop Experiment
                    </button>
                  </div>
                </div>

                {expTab === "preview" && (
                  <>
                    <div className="canvas-wrap">
                      {!lastMeta && <span className="canvas-hint">No frame yet — configure a camera and press Start Camera</span>}
                      <canvas ref={previewCanvasRef} className={fitWindow ? "fit" : ""} />
                    </div>
                    <div className="toolbar" style={{ marginTop: 6 }}>
                      <button disabled title={PENDING.monitoring}>Overlay: Both</button>
                      <span className="legend">
                        <span className="chip"><span className="swatch" style={{ background: "#2b6cb0" }} /> Target</span>
                        <span className="chip"><span className="swatch" style={{ background: "#1a7f37" }} /> Valid</span>
                        <span className="chip"><span className="swatch" style={{ background: "#b42318" }} /> Invalid</span>
                      </span>
                      <button
                        onClick={async () => {
                          const res = await bridge.setBackgroundFromCurrentFrame();
                          append(res.ok ? "background captured from current frame" : `set background failed: ${res.message}`);
                          await refreshConfig();
                        }}
                        disabled={!running}
                        title={running ? "Capture the current frame as the processing background" : "Camera is not running"}
                      >
                        Set Background
                      </button>
                      <button
                        onClick={async () => {
                          await bridge.clearBackgroundImage();
                          append("background cleared");
                          await refreshConfig();
                        }}
                        disabled={!backgroundSet}
                        title={backgroundSet ? undefined : "No background is set"}
                      >
                        Clear Background
                      </button>
                      <label title="Auto background is configured via auto_background_* in the App config">
                        <input type="checkbox" disabled checked={false} /> Auto
                      </label>
                      <button
                        onClick={async () => {
                          setRoiFields({ x: "0", y: "0", w: "0", h: "0" });
                          await bridge.setProcessingRoi(0, 0, 0, 0);
                          await refreshConfig();
                        }}
                        disabled={!ready}
                      >
                        Clear ROI
                      </button>
                      <button disabled title={PENDING.saveBuffer}>Save Buffer</button>
                      <button onClick={onToggleRecord} disabled={!running} title={running ? "Record raw frames to an HDF5 file" : "Camera is not running"}>
                        {recording ? "Stop Recording" : "Record"}
                      </button>
                      <button onClick={() => setFitWindow((f) => !f)}>{fitWindow ? "Fit: Window" : "Fit: 1:1"}</button>
                    </div>
                    <input type="range" className="scrub" disabled title={PENDING.saveBuffer} aria-label="Preview buffer scrub (not bridged)" />

                    <div className="subtabs" style={{ marginTop: 8 }} role="tablist" aria-label="Configuration">
                      <button className={configTab === "app" ? "active" : ""} onClick={() => setConfigTab("app")}>
                        App config (config.json)
                      </button>
                      <button className={configTab === "script" ? "active" : ""} onClick={() => setConfigTab("script")}>
                        Camera script
                      </button>
                    </div>
                    <div className="subtab-body">
                      {configTab === "app" && (
                        <>
                          <div className="toolbar">
                            <button onClick={refreshConfig} disabled={!ready} title="Reload the live config from the backend">
                              Reload
                            </button>
                            <button className="btn" onClick={onApplyConfigJson} disabled={!ready || !configDirty} title={configDirty ? "Merge-apply the edited document" : "No edits to apply"}>
                              Apply
                            </button>
                            <label>
                              Profile:{" "}
                              <select disabled title={PENDING.profiles}>
                                <option>&lt;no prof&gt;</option>
                              </select>
                            </label>
                            <button disabled title={PENDING.profiles}>Save Profile</button>
                            <button disabled title={PENDING.profiles}>Show Diff</button>
                            <span className="mono right" title="Active processing core identity (backend-owned trust)">
                              core {coreStatus?.valid ? `v${coreStatus.active_version} (${coreStatus.source})` : "—"}
                              {coreStatus?.valid && !coreStatus.pin_satisfied
                                ? ` · PIN NOT SATISFIED (requires ${coreStatus.required_version})`
                                : ""}
                            </span>
                          </div>
                          <div className="config-grid">
                            <div className="config-group" style={{ flex: 2 }}>
                              <h5>Live config document (merge-applied on Apply)</h5>
                              <textarea
                                className="script-editor"
                                style={{ minHeight: 160 }}
                                value={configText}
                                onChange={(e) => {
                                  setConfigText(e.target.value);
                                  setConfigDirty(true);
                                }}
                                aria-label="Processing configuration JSON"
                              />
                            </div>
                            <div className="config-group">
                              <h5>realtime_processing</h5>
                              <div className="row">
                                <label>
                                  <input
                                    type="checkbox"
                                    checked={procEnabled}
                                    onChange={(e) => setProcEnabled(e.target.checked)}
                                  />{" "}
                                  realtime processing
                                </label>
                              </div>
                              <div className="row">
                                <label>
                                  px→µm{" "}
                                  <input
                                    type="text"
                                    style={{ width: 70 }}
                                    value={pixelToMicron}
                                    onChange={(e) => setPixelToMicron(e.target.value)}
                                  />
                                </label>
                                <button className="btn" onClick={onApplyProcessing} disabled={!ready} title={ready ? undefined : "Backend is not initialized"}>
                                  Apply
                                </button>
                              </div>
                              {stats?.valid && (
                                <p className="mono">
                                  algo {stats.algo_fps1s.toFixed(1)} · valid {stats.valid_fps1s.toFixed(1)} · invalid{" "}
                                  {stats.invalid_fps1s.toFixed(1)} fps · px→µm {stats.pixel_to_micron}
                                </p>
                              )}
                              <p className="mono">background: {backgroundSet ? "set" : "not set"}</p>
                            </div>
                          </div>
                        </>
                      )}
                      {configTab === "script" && (
                        <>
                          <div className="toolbar">
                            <button disabled title={PENDING.script}>Reset</button>
                            <button disabled title={PENDING.script}>Save</button>
                            <button disabled title={PENDING.script}>Apply to Camera</button>
                          </div>
                          <textarea
                            className="script-editor"
                            disabled
                            title={PENDING.script}
                            value={"// Camera script editing is not bridged yet — BE-2 (#272)."}
                            readOnly
                          />
                        </>
                      )}
                    </div>
                  </>
                )}

                {expTab === "monitoring" && (
                  <>
                    <div className="toolbar">
                      <button
                        onClick={async () => {
                          const res = await bridge.monitoringClear();
                          append(res.ok ? "monitoring buffers cleared" : `clear failed: ${res.message}`);
                        }}
                        disabled={!ready}
                      >
                        Clear Buffer
                      </button>
                      {/* UX-9: trigger tests are commissioning controls, gated
                          behind Service mode; a running periodic test stays
                          stoppable in Operator mode as a safety fallback. */}
                      {operatingMode === "service" ? (
                        <>
                          <label className="arm-toggle" title="Arm the hardware-actuating trigger controls">
                            <input
                              type="checkbox"
                              checked={triggerArmed}
                              onChange={(e) => setTriggerArmed(e.target.checked)}
                              disabled={expActive || !trigStatus?.camera_attached}
                            />{" "}
                            Arm
                          </label>
                          <button
                            onClick={async () => {
                              const res = await bridge.triggerManualPulse();
                              append(res.ok ? `manual sort pulse fired (${pulseUs} µs)` : `sort trigger failed: ${res.message}`);
                              setTriggerArmed(false);
                            }}
                            disabled={!actuateCheck.allowed}
                            title={actuateCheck.allowed ? `Fire one manual sorter pulse (${pulseUs} µs)` : actuateCheck.reason}
                          >
                            Sort Trigger
                          </button>
                          <label>
                            <input
                              type="number"
                              style={{ width: 64 }}
                              value={pulseUs}
                              onChange={(e) => setPulseUs(e.target.value)}
                              aria-label="Pulse duration (µs)"
                            />{" "}
                            µs
                          </label>
                          <button
                            className="btn"
                            onClick={async () => {
                              const res = await bridge.triggerSetPulseDuration(Number(pulseUs) || 1);
                              append(res.ok ? `pulse duration ${pulseUs} µs` : `pulse duration failed: ${res.message}`);
                            }}
                            disabled={!ready}
                          >
                            Set Pulse
                          </button>
                          <button
                            onClick={async () => {
                              const active = trigStatus?.periodic_active;
                              const res = active
                                ? await bridge.triggerPeriodicStop()
                                : await bridge.triggerPeriodicStart(Number(periodicMs) || 1000);
                              append(
                                res.ok
                                  ? active
                                    ? "periodic test stopped"
                                    : `periodic test started (${periodicMs} ms)`
                                  : `periodic test failed: ${res.message}`,
                              );
                              setTriggerArmed(false);
                              setTrigStatus(await bridge.fetchTriggerStatus());
                            }}
                            disabled={trigStatus?.periodic_active ? !stopPeriodicCheck.allowed : !startPeriodicCheck.allowed}
                            title={
                              trigStatus?.periodic_active
                                ? "Stop the running periodic test"
                                : startPeriodicCheck.allowed
                                  ? `Start a periodic test every ${periodicMs} ms`
                                  : startPeriodicCheck.reason
                            }
                          >
                            {trigStatus?.periodic_active ? "Stop Periodic Test" : "Periodic Test"}
                          </button>
                          <label>
                            <input
                              type="number"
                              style={{ width: 72 }}
                              value={periodicMs}
                              onChange={(e) => setPeriodicMs(e.target.value)}
                              disabled={trigStatus?.periodic_active}
                              aria-label="Periodic interval (ms)"
                            />{" "}
                            ms
                          </label>
                        </>
                      ) : trigStatus?.periodic_active ? (
                        <button
                          onClick={async () => {
                            const res = await bridge.triggerPeriodicStop();
                            append(res.ok ? "periodic test stopped" : `periodic test failed: ${res.message}`);
                            setTrigStatus(await bridge.fetchTriggerStatus());
                          }}
                          title="Stop the running periodic test"
                        >
                          Stop Periodic Test
                        </button>
                      ) : (
                        <span className="commissioning-hint mono">
                          Trigger tests are in Service / Commissioning mode
                        </span>
                      )}
                      <span className="mono right">
                        triggers {trigStatus?.trigger_count ?? 0} · buffered v{monSnapshot?.valid_held ?? 0}/i
                        {monSnapshot?.invalid_held ?? 0} · evicted{" "}
                        {Math.max(
                          0,
                          (monSnapshot?.valid_appended ?? 0) +
                            (monSnapshot?.invalid_appended ?? 0) -
                            (monSnapshot?.valid_held ?? 0) -
                            (monSnapshot?.invalid_held ?? 0),
                        )}
                      </span>
                    </div>
                    <div className="config-grid" style={{ flex: 1 }}>
                      <div className="config-group" title={PENDING.monitoring}>
                        <h5>Deformability vs Area (µm²)</h5>
                        <p className="pending-note">
                          Chart rendering lands with UI-3 (#268); the bounded metric rows below are the live chart inputs.
                        </p>
                      </div>
                      <div className="config-group" title={PENDING.monitoring}>
                        <h5>Ring Width Distribution</h5>
                        <p className="pending-note">
                          Chart rendering lands with UI-3 (#268); ring-ratio inputs are in the metric rows below.
                        </p>
                      </div>
                      <div className="config-group" title={PENDING.config}>
                        <h5>Tune Params</h5>
                        <p className="pending-note">
                          Filter thresholds / target group / multi-image editing lands with the config round-trip — BE-3 (#273).
                        </p>
                      </div>
                    </div>
                    <div className="table-panel" style={{ maxHeight: 180, overflow: "auto" }}>
                      <table className="metrics-table">
                        <thead>
                          <tr>
                            <th>Frame</th>
                            <th>Object</th>
                            <th>Track</th>
                            <th>Valid</th>
                            <th>Target</th>
                            <th>Area (µm²)</th>
                            <th>Deformability</th>
                            <th>Ring ratio</th>
                            <th>E (kPa)</th>
                          </tr>
                        </thead>
                        <tbody>
                          {(monSnapshot?.rows ?? []).slice(-15).map((r) => (
                            <tr key={`${r.frame_index}:${r.object_id}`}>
                              <td>{r.frame_index}</td>
                              <td>{r.object_id}</td>
                              <td>{r.track_id}</td>
                              <td>{r.valid ? "yes" : "no"}</td>
                              <td>{r.target_group ? "yes" : "no"}</td>
                              <td>{r.area.toFixed(1)}</td>
                              <td>{r.deformability.toFixed(3)}</td>
                              <td>{r.ring_ratio.toFixed(3)}</td>
                              <td>{r.youngs_modulus.toFixed(2)}</td>
                            </tr>
                          ))}
                          {(monSnapshot?.rows?.length ?? 0) === 0 && (
                            <tr>
                              <td colSpan={9} style={{ color: "#777" }}>
                                No monitoring rows yet — rows appear while capture + realtime processing run with Monitoring
                                visible.
                              </td>
                            </tr>
                          )}
                        </tbody>
                      </table>
                    </div>
                  </>
                )}
              </>
            )}

            {/* ---- Review ---- */}
            {tab === "review" && (
              <>
                <div className="toolbar">
                  <button onClick={onSelectHdf} disabled={!ready} title={ready ? undefined : "Backend is not initialized"}>
                    Select HDF File…
                  </button>
                  <button disabled title="Loading a new file replaces the current one">Close File</button>
                  <button
                    onClick={onExportCsv}
                    disabled={!reviewMeta?.file_open}
                    title={reviewMeta?.file_open ? "Export frame/object metrics as a cancellable job" : "No file loaded"}
                  >
                    Export Metrics to CSV…
                  </button>
                  <button disabled title={PENDING.review}>Export All…</button>
                  <button disabled title={PENDING.review}>Batch Metrics…</button>
                  <button disabled title={PENDING.review}>Regenerate masks…</button>
                  <span className="legend">
                    <span className="chip"><span className="swatch" style={{ background: "#2b6cb0" }} /> Target</span>
                    <span className="chip"><span className="swatch" style={{ background: "#1a7f37" }} /> Valid</span>
                    <span className="chip"><span className="swatch" style={{ background: "#b42318" }} /> Invalid</span>
                  </span>
                  <span className="path-label right">
                    {reviewing
                      ? `${reviewPath}${reviewMeta?.valid ? ` · ${reviewMeta.recording_file ? "recording" : "experiment"} · valid ${reviewMeta.total_valid}, invalid ${reviewMeta.total_invalid}${reviewMeta.has_core_identity ? ` · core v${reviewMeta.core_version}` : ""}` : ""}`
                      : "No file selected"}
                  </span>
                </div>
                <div className="subtabs" role="tablist" aria-label="Review views">
                  <button className={reviewTab === "raw" ? "active" : ""} onClick={() => setReviewTab("raw")}>
                    Raw Frames
                  </button>
                  <button
                    className={reviewTab === "valid" ? "active" : ""}
                    disabled={!reviewMeta?.valid_images.present}
                    title={reviewMeta?.valid_images.present ? undefined : "No valid-frame images in this file"}
                    onClick={async () => {
                      setReviewTab("valid");
                      setReviewImgIndex(0);
                      await loadMetricsPage(true, 0);
                      await drawReviewImage(0, 0);
                    }}
                  >
                    Valid Frames
                  </button>
                  <button
                    className={reviewTab === "invalid" ? "active" : ""}
                    disabled={!reviewMeta?.invalid_images.present}
                    title={reviewMeta?.invalid_images.present ? undefined : "No invalid-frame images in this file"}
                    onClick={async () => {
                      setReviewTab("invalid");
                      setReviewImgIndex(0);
                      await loadMetricsPage(false, 0);
                      await drawReviewImage(1, 0);
                    }}
                  >
                    Invalid Frames
                  </button>
                  <button disabled title="Chart rendering lands with UI-4 (#269)">Charts</button>
                </div>
                <div className="subtab-body">
                  <div className="review-split">
                    <div className="frames">
                      <div className="canvas-wrap">
                        {!reviewing && <span className="canvas-hint">No recording loaded — Select HDF File…</span>}
                        <canvas ref={reviewCanvasRef} className={fitWindow ? "fit" : ""} />
                      </div>
                      {reviewing && reviewTab === "raw" && range.count > 0 && (
                        <>
                          <input
                            type="range"
                            className="scrub"
                            min={range.earliest}
                            max={range.latest}
                            value={reviewIndex}
                            onChange={(e) => onScrub(Number(e.target.value))}
                            aria-label="Frame scrubber"
                          />
                          <span className="mono">
                            frame {reviewIndex} of [{range.earliest}…{range.latest}] ({range.count} available)
                          </span>
                        </>
                      )}
                      {reviewing && (reviewTab === "valid" || reviewTab === "invalid") && (
                        <>
                          <input
                            type="range"
                            className="scrub"
                            min={0}
                            max={Math.max(
                              0,
                              (reviewTab === "valid"
                                ? reviewMeta?.valid_images.count ?? 0
                                : reviewMeta?.invalid_images.count ?? 0) - 1,
                            )}
                            value={reviewImgIndex}
                            onChange={async (e) => {
                              const idx = Number(e.target.value);
                              setReviewImgIndex(idx);
                              await drawReviewImage(reviewTab === "valid" ? 0 : 1, idx);
                            }}
                            aria-label="Review image scrubber"
                          />
                          <span className="mono">
                            image {reviewImgIndex + 1} of{" "}
                            {reviewTab === "valid"
                              ? reviewMeta?.valid_images.count ?? 0
                              : reviewMeta?.invalid_images.count ?? 0}
                          </span>
                        </>
                      )}
                    </div>
                    <div className="table-panel">
                      <table className="metrics-table">
                        <thead>
                          <tr>
                            <th>Index</th>
                            <th>Object Id</th>
                            <th>Track Id</th>
                            <th>Area (px²)</th>
                            <th>Deformability</th>
                            <th>Ring ratio</th>
                            <th>E (kPa)</th>
                          </tr>
                        </thead>
                        <tbody>
                          {(metricsPage?.rows ?? []).map((r) => (
                            <tr key={`${r.frame_index}:${r.object_id}`}>
                              <td>{r.frame_index}</td>
                              <td>{r.object_id}</td>
                              <td>{r.track_id}</td>
                              <td>{r.area.toFixed(1)}</td>
                              <td>{r.deformability.toFixed(3)}</td>
                              <td>{r.ring_ratio.toFixed(3)}</td>
                              <td>{r.youngs_modulus.toFixed(2)}</td>
                            </tr>
                          ))}
                          {(metricsPage?.rows?.length ?? 0) === 0 && (
                            <tr>
                              <td colSpan={7} style={{ color: "#777" }}>
                                {reviewing ? "No metric rows in this table." : "Load a file to see frame/object metrics."}
                              </td>
                            </tr>
                          )}
                        </tbody>
                      </table>
                      {reviewing && (metricsPage?.total ?? 0) > METRICS_PAGE_SIZE && (
                        <div className="toolbar" style={{ padding: 4 }}>
                          <button
                            className="btn"
                            disabled={metricsOffset === 0}
                            onClick={() => loadMetricsPage(reviewTab !== "invalid", Math.max(0, metricsOffset - METRICS_PAGE_SIZE))}
                          >
                            ◀ Prev
                          </button>
                          <span className="mono">
                            {metricsOffset + 1}–{Math.min(metricsPage?.total ?? 0, metricsOffset + METRICS_PAGE_SIZE)} of {metricsPage?.total ?? 0}
                          </span>
                          <button
                            className="btn"
                            disabled={metricsOffset + METRICS_PAGE_SIZE >= (metricsPage?.total ?? 0)}
                            onClick={() => loadMetricsPage(reviewTab !== "invalid", metricsOffset + METRICS_PAGE_SIZE)}
                          >
                            Next ▶
                          </button>
                        </div>
                      )}
                    </div>
                  </div>
                </div>
              </>
            )}
          </div>
        </main>
      </div>

      {/* ---- Status bar ---- */}
      {/* ---- UX-8 persistent active-context bar (all stages) ---- */}
      <div className="context-bar" role="region" aria-label="Active context">
        {contextBar.segments.map((s) => {
          const label = `${s.label}: ${s.value} (${SEG_STATUS_LABEL[s.status]})`;
          const clickable = !!s.tab;
          return (
            <button
              key={s.id}
              type="button"
              className={`context-seg ${s.status}${clickable ? " clickable" : ""}`}
              title={s.detail}
              aria-label={label}
              disabled={!clickable}
              onClick={clickable ? () => setTab(s.tab!) : undefined}
            >
              <span className={`seg-dot ${s.status}`} aria-hidden="true" />
              <span className="seg-text">
                <span className="seg-label">{s.label}</span>
                <span className="seg-value">{s.value}</span>
              </span>
            </button>
          );
        })}
      </div>

      <footer className="statusbar">
        <span>
          Bridge ABI: {abi ?? "…"} · backend: {ready ? "ready" : "not initialized"}
          {reviewing ? " · reviewing" : ""}
          {recording ? " · recording" : ""}
        </span>
        <button className="log-toggle" onClick={() => setShowLog((s) => !s)}>
          Log {showLog ? "▾" : "▸"} ({log.length})
        </button>
        <span className="metrics">
          Display={displayFps.toFixed(1)} fps | Algo={algoFps.toFixed(1)}/s | Valid={validFps.toFixed(1)}/s | Invalid=
          {invalidFps.toFixed(1)}/s | Camera={running ? "running" : camStatus}, {dataRate.toFixed(1)} MB/s | Experiment:{" "}
          {(EXPERIMENT_STATE_NAMES[expState] ?? "Inactive").toLowerCase()}
          {expActive ? ` (buffered ${(expStatus?.valid_buffered ?? 0) + (expStatus?.invalid_buffered ?? 0)})` : ""}
        </span>
      </footer>
      {showLog && (
        <div className="log-drawer" role="log">
          {log.length === 0 ? <div>no log entries</div> : log.map((l, i) => <div key={i}>{l}</div>)}
        </div>
      )}

      {/* ---- Configure Mock modal ---- */}
      {showMockConfig && (
        <div className="modal-backdrop" onClick={() => setShowMockConfig(false)}>
          <div className="modal" onClick={(e) => e.stopPropagation()} role="dialog" aria-label="Configure mock camera">
            <h3>Configure Mock Camera</h3>
            <div className="row">
              <input
                type="text"
                placeholder="mock frame directory (folder of .tiff/.png)"
                value={frameDir}
                onChange={(e) => setFrameDir(e.target.value)}
              />
              <button
                className="btn"
                onClick={async () => {
                  const picked = await open({ directory: true, title: "Select mock frame directory" });
                  if (typeof picked === "string") setFrameDir(picked);
                }}
              >
                Browse…
              </button>
            </div>
            <div className="row">
              <label>
                Frame interval{" "}
                <input
                  type="number"
                  style={{ width: 80, flex: "none" }}
                  value={intervalMs}
                  onChange={(e) => setIntervalMs(e.target.value)}
                />{" "}
                ms
              </label>
              <label>
                <input type="checkbox" checked={loopFiles} onChange={(e) => setLoopFiles(e.target.checked)} /> loop files
              </label>
            </div>
            <div className="actions">
              <button className="btn" onClick={() => setShowMockConfig(false)}>Cancel</button>
              <button className="btn" onClick={onConfigureMock} disabled={!frameDir} title={frameDir ? undefined : "Pick a frame directory first"}>
                Apply
              </button>
            </div>
          </div>
        </div>
      )}

      {/* ---- About modal ---- */}
      {showAbout && (
        <div className="modal-backdrop" onClick={() => setShowAbout(false)}>
          <div className="modal" onClick={(e) => e.stopPropagation()} role="dialog" aria-label="About">
            <h3>MIB Studio (React + Tauri)</h3>
            <p>
              Desktop shell for the Qt-free MIB backend (epic #246).
              <br />
              Bridge ABI version: {abi ?? "unknown"} · backend {ready ? "initialized" : "not initialized"}.
            </p>
            <div className="actions">
              <button className="btn" onClick={() => setShowAbout(false)}>Close</button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}
