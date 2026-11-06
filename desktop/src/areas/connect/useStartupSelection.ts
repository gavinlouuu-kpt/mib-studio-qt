// Connect stage (epic #863, B1): what happens after the startup camera step. The backend's startup coordinator
// (StartupDiscoveryCoordinator) scans and selects the unique identified camera; this hook tells the operator what
// it found, as Qt's DeviceInitManager does: lists refreshed once per camera job, the auto-selected camera
// highlighted with its sub-tab open and the stage moved to Overview, "No cameras found." with the check-power
// dialog, "Multiple cameras found; select one and click Connect.", "Camera discovery incomplete: <reason>".
// It also starts the startup selection on instruments whose Hardware controls (which start it on the others) are
// not shown. Nanopositioner outcomes stay with the hardware area (#868).
import { useCallback, useEffect, useRef } from "react";
import { bridge } from "../../bridge";
import { isTerminalDiscoveryState, toCameraDiscovery } from "../../discovery";
import { resolveAutoSelect, saveAutoSelect } from "../../startupPreference";
import type { MonitoringState } from "../monitoring/useMonitoring";
import type { ShellState } from "../shell/useShell";
import {
  connectedNote, discoveryStatusText, preselectDevices, rowOfSelection, startupCameraOutcome, type PickedDevices,
} from "./connectFlow";
import type { ConnectState } from "./useConnect";

type StartupSelectionDeps = Pick<ShellState, "ready" | "platform" | "caps" | "pz7035" | "append" | "setTab">
  & Pick<ConnectState, "setDiscovery" | "setCamSelection" | "setPickedDevices" | "setConnectTab" | "setActionNote" | "setScanNote"
    | "setNoCameraDialog" | "autoSelectStored" | "setAutoSelectStored">
  & Pick<MonitoringState, "expActive">;

const POLL_MS = 500;
const FIRST_READING_PATIENCE_MS = 2000;

export function useStartupSelection({
  ready, platform, caps, pz7035, append, setTab, setDiscovery, setCamSelection, setPickedDevices, setConnectTab,
  setActionNote, setScanNote, setNoCameraDialog, autoSelectStored, setAutoSelectStored, expActive,
}: StartupSelectionDeps) {
  const autoSelect = resolveAutoSelect(autoSelectStored, pz7035);
  const live = useRef({ autoSelect, expActive, hardwareControls: caps.autofocus });
  live.current = { autoSelect, expActive, hardwareControls: caps.autofocus };

  // The operator's opt-out (and opt-in again), shown on the Connect stage where the Hardware controls are absent.
  const onAutoSelectChange = useCallback((value: boolean) => {
    setAutoSelectStored(value);
    if (!saveAutoSelect(localStorage, value)) append("Startup preference could not be saved.");
  }, [append, setAutoSelectStored]);

  // The report below treats a job that had ended before the first status reading as already reported (a webview reload). So
  // the selection is started only after that first reading: a scan that ends within milliseconds (no SDK on the PC) is not
  // taken for an old one.
  const firstReading = useRef<{ promise: Promise<void>; done: () => void }>();
  const freshFirstReading = () => {
    let done = () => {};
    const promise = new Promise<void>((resolve) => { done = resolve; });
    firstReading.current = { promise, done };
  };
  if (!firstReading.current) freshFirstReading();

  // Start the startup selection once per backend-ready session (#852). With autofocus the Hardware controls own it:
  // they first install the remembered nanopositioner preference.
  const started = useRef(false);
  useEffect(() => {
    if (!ready) { started.current = false; freshFirstReading(); return; }
    if (started.current || !platform) return;
    started.current = true;
    if (live.current.hardwareControls || !live.current.autoSelect || live.current.expActive) return;
    const reading = firstReading.current!.promise;
    const patience = new Promise<void>((resolve) => { window.setTimeout(resolve, FIRST_READING_PATIENCE_MS); });
    void Promise.race([reading, patience])
      .then(() => bridge.startupDiscoveryRun("start"))
      .then((r) => append(r.message))
      .catch((e) => append(`startup selection: ${e}`));
  }, [ready, platform, append]);

  // "Try again" in the no-camera dialog: Qt's tryAutoConnect (the immediate camera step).
  const onTryCameraAgain = useCallback(async () => {
    setNoCameraDialog(false);
    try {
      const r = await bridge.startupDiscoveryRun("camera");
      if (!r.accepted) setActionNote(r.message);
      append(r.message);
    } catch (e) {
      setActionNote(`Try again: ${e}`);
      append(`startup selection: ${e}`);
    }
  }, [append, setActionNote, setNoCameraDialog]);

  // Every camera job of the startup coordinator (startup, Try again, Retry camera selection) is reported once when it ends.
  useEffect(() => {
    if (!ready) return;
    let alive = true, polling = false, primed = false;
    let lastJob = "0", wasConfigured = false;
    const reading = firstReading.current!;
    const poll = async () => {
      if (polling) return;
      polling = true;
      try {
        const status = await bridge.startupDiscoveryStatus();
        if (!alive || !status.valid) return;
        const job = status.camera.job_id;
        const configuredBefore = wasConfigured;
        wasConfigured = status.camera_configured;
        if (!primed) {
          // A job that ended before this view looked (a webview reload) was reported already.
          primed = true;
          if (job !== "0" && !status.camera_running) lastJob = job;
          return;
        }
        if (job === "0" || job === lastJob || status.camera_running || !isTerminalDiscoveryState(status.camera.state ?? -1)) return;
        lastJob = job;
        const snapshot = await bridge.fetchDeviceDiscovery(job);
        if (!alive) return;
        const found = toCameraDiscovery(snapshot);
        const selection = await bridge.fetchCameraSelection();
        if (!alive) return;
        setDiscovery(found);
        setCamSelection(selection);
        setPickedDevices((previous) => preselectDevices(found, previous));
        setScanNote(discoveryStatusText(found));
        if (status.camera_configured) {
          if (configuredBefore) return; // already configured: the step was skipped
          // Selected: highlight the camera in its list, open that sub-tab, then move to Overview (CONNECT-027).
          const row = rowOfSelection(found, selection);
          const label = row?.label || selection.label || "camera";
          if (row) {
            setPickedDevices((previous): PickedDevices => ({ ...previous, [row.tab]: row.key }));
            setConnectTab(row.tab);
          }
          setActionNote(connectedNote(label));
          append(`startup: selected ${label}`);
          setTab((t) => (t === "connect" ? "overview" : t));
          return;
        }
        const outcome = startupCameraOutcome(snapshot);
        if (outcome.kind === "one") return; // the unique camera was refused by the backend; its message is in the log
        setActionNote(outcome.text);
        append(`startup: ${outcome.text}`);
        if (outcome.kind === "none") {
          // Qt (MainWindow::onNoCamerasFound): force the Connect stage and say what to check.
          setTab("connect");
          setNoCameraDialog(true);
        }
      } catch {
        /* the backend is gone or the job was evicted: the next poll, or the operator's Refresh, tells */
      } finally {
        polling = false;
        reading.done(); // the first reading is in (or failed): the selection may start
      }
    };
    void poll();
    const timer = window.setInterval(() => void poll(), POLL_MS);
    return () => { alive = false; window.clearInterval(timer); };
  }, [ready, append, setDiscovery, setCamSelection, setPickedDevices, setConnectTab, setActionNote, setScanNote, setNoCameraDialog, setTab]);

  return { autoSelect, onAutoSelectChange, onTryCameraAgain };
}
export type StartupSelectionModel = ReturnType<typeof useStartupSelection>;
