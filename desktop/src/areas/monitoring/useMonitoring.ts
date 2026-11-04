// Experiment start/stop and Monitoring (epic #866): the backend-owned experiment lifecycle, the
// last run's outcome, the Monitoring snapshot polling and the SATA SSD record store status.
// Moved from App.tsx without change (#869 wave 0); studio/useStudioController.ts calls these
// hooks in App.tsx's original effect order.
import { useCallback, useEffect, useMemo, useReducer, useRef, useState } from "react";
import { bridge, type ExperimentStatus, type MonitoringSnapshot, type RunAccounting } from "../../bridge";
import { EXPERIMENT_STATES } from "../../bridgeContract";
import { startExperiment } from "../../experimentStart";
import { describeRunOutcome, runKey } from "../../runOutcome";
import type { SsdStatus } from "../../ssdView";
import { isRemote } from "../../transport";
import { confirm, save } from "../../transport/dialogs";
import { H5_FILTER } from "../../studio/constants";
import { initialTuneState, tuneReducer, type TuneFieldId } from "./tuneDraft";
import type { ConfigState, CoresModel } from "../config/useConfig";
import type { ConnectState } from "../connect/useConnect";
import type { HardwareState } from "../hardware/useHardware";
import type { ShellState } from "../shell/useShell";
import { MONITORING_FETCH_ROWS, sameSnapshot } from "./monitoringChartModel";
import {
  DEFAULT_MONITORING_SETTINGS, ringRatioRangeOfConfig, withHistogramXRange, type MonitoringSettings,
} from "./monitoringSettings";

export const EXPERIMENT_STATE_NAMES: Record<number, string> = {
  [EXPERIMENT_STATES.Idle]: "Inactive",
  [EXPERIMENT_STATES.Starting]: "Starting",
  [EXPERIMENT_STATES.Active]: "Active",
  [EXPERIMENT_STATES.Stopping]: "Stopping",
  [EXPERIMENT_STATES.Failed]: "Failed",
};

export function useMonitoringState() {
  const [expTab, setExpTab] = useState<"preview" | "monitoring" | "files" | "recordings">("preview");

  // Experiment (bridge schema v5, BE-4 — backend-owned lifecycle).
  const [expStatus, setExpStatus] = useState<ExperimentStatus | null>(null);

  // Monitoring + trigger (bridge schema v6, BE-5).
  const [monSnapshot, setMonSnapshot] = useState<MonitoringSnapshot | null>(null);
  // Monitoring Settings (MON-082): the fixed chart axes and the histogram bin width. A view preference
  // kept in memory, like Qt's; the dialog is areas/monitoring/MonitoringSettingsDialog.tsx.
  const [monSettings, setMonSettings] = useState<MonitoringSettings>(DEFAULT_MONITORING_SETTINGS);
  const [showMonitoringSettings, setShowMonitoringSettings] = useState(false);
  const experimentPending = useRef(false);
  const [experimentRequestBusy, setExperimentRequestBusy] = useState(false);
  const [readinessMessage, setReadinessMessage] = useState("");
  // Non-blocking readiness warnings shown at run start (e.g. recording to RAM, #501).
  const [startNotice, setStartNotice] = useState("");
  const expState = expStatus?.valid ? expStatus.state : EXPERIMENT_STATES.Idle;
  // How the last finished run ended (#549): completion and loss counts, not only "finalized".
  // The run's reconciled accounting (ABI 31) gives the true admitted-frame denominator; the status
  // alone only knows the rows it saved.
  const [runAccounting, setRunAccounting] = useState<RunAccounting | null>(null);
  const runOutcome = describeRunOutcome(expStatus, runAccounting);
  const finishedRun = runKey(expStatus);
  const loggedRun = useRef("");
  const elapsedWallSeconds = expStatus?.valid && BigInt(expStatus.start_time_ns) > 0n
    ? Number(((BigInt(expStatus.end_time_ns) || BigInt(Date.now()) * 1000000n) - BigInt(expStatus.start_time_ns)) / 1000000000n) : null;
  const expActive = expState === EXPERIMENT_STATES.Starting || expState === EXPERIMENT_STATES.Active || expState === EXPERIMENT_STATES.Stopping;
  // The tune panel's draft (#866 MON-069..081; tuneDraft.ts). `tuneDraft` and `setTuneField` are what the
  // scatter's gate overlay (MON-064..068) reads and writes, so a gate dragged on the chart is the same
  // draft as the panel's fields and goes through the same Apply.
  const [tune, dispatchTune] = useReducer(tuneReducer, undefined, initialTuneState);
  const tuneDraft = tune.draft;
  const setTuneField = useCallback((field: TuneFieldId, value: unknown) => dispatchTune({ type: "edit", field, value }), []);
  return {
    tune, dispatchTune, tuneDraft, setTuneField,
    expTab, setExpTab, expStatus, setExpStatus, monSnapshot, setMonSnapshot, monSettings, setMonSettings,
    showMonitoringSettings, setShowMonitoringSettings,
    experimentPending, experimentRequestBusy, setExperimentRequestBusy, readinessMessage, setReadinessMessage,
    startNotice, setStartNotice, expState, runAccounting, setRunAccounting, runOutcome, finishedRun, loggedRun,
    elapsedWallSeconds, expActive,
  };
}
export type MonitoringState = ReturnType<typeof useMonitoringState>;

type MonitoringPollingDeps = Pick<ShellState, "ready" | "tab"> & Pick<MonitoringState, "expTab" | "setMonSnapshot">
  & Pick<HardwareState, "setTrigStatus">;

export function useMonitoringPolling({ ready, tab, expTab, setMonSnapshot, setTrigStatus }: MonitoringPollingDeps) {
  // Monitoring is visibility-gated (BE-5): accumulation and its per-frame
  // image clones run only while the Monitoring view is actually shown.
  const monitoringVisible = tab === "experiment" && expTab === "monitoring";
  useEffect(() => {
    if (!ready) return;
    bridge.monitoringSetActive(monitoringVisible).catch(() => {});
    if (!monitoringVisible) return;
    let polling = false, disposed = false;
    const id = window.setInterval(async () => {
      if (polling) return; polling = true;
      try {
        // The plotted window (1000 valid rows, MON-036) plus the latest-rows table, not a fixed 200 rows.
        const snapshot = await bridge.fetchMonitoringSnapshot(MONITORING_FETCH_ROWS);
        const trigger = await bridge.fetchTriggerStatus();
        // A poll that finds nothing new keeps the previous snapshot: no re-render, no redraw.
        if (!disposed) { setMonSnapshot((previous) => (sameSnapshot(previous, snapshot) ? previous : snapshot)); setTrigStatus(trigger); }
      } catch {
        /* backend gone — next tick will surface it */
      } finally { polling = false; }
    }, 500);
    return () => { disposed = true; window.clearInterval(id); };
  }, [monitoringVisible, ready]);
  return { monitoringVisible };
}

type MonitoringConfigRangeDeps = Pick<MonitoringState, "setMonSettings"> & Pick<ConfigState, "liveDraft">
  & { monitoringVisible: boolean };

// MON-050: the histogram's X range is the runtime config's ring_ratio_min/max, taken when the Monitoring
// sub-tab is shown (overwriting a range set in the dialog, as Qt's showEvent does) and again whenever
// the config's range changes, which is what an Apply or a Revert of the processing configuration does.
// The baseline is the last config the backend gave (liveConfigDraft), not an unapplied edit.
export function useMonitoringConfigRange({ monitoringVisible, liveDraft, setMonSettings }: MonitoringConfigRangeDeps) {
  const range = useMemo(() => ringRatioRangeOfConfig(liveDraft.baseline), [liveDraft.baseline]);
  const min = range?.min, max = range?.max;
  useEffect(() => {
    if (!monitoringVisible || min === undefined || max === undefined) return;
    setMonSettings((current) => withHistogramXRange(current, min, max));
  }, [monitoringVisible, min, max]);
}

export function useSsdStatus({ ready }: Pick<ShellState, "ready">) {
  // The SATA SSD record store (#667 S1, read only): polled on its own (the server answers from a cache, no child process per call) while connected to a
  // server; without MIB_PZREC and MIB_SSD_IMAGE it says ABSENT and the strip and the Recordings tab stay hidden.
  const [ssd, setSsd] = useState<SsdStatus | null>(null);
  const ssdConfigured = !!ssd?.configured;
  useEffect(() => {
    if (!ready || !isRemote) { setSsd(null); return; }
    let cancelled = false;
    const poll = () => { void bridge.fetchSsdStatus().then((s) => { if (!cancelled) setSsd(s); }).catch(() => undefined); };
    poll();
    const id = window.setInterval(poll, 2000);
    return () => { cancelled = true; window.clearInterval(id); };
  }, [ready, isRemote]);
  return { ssd, ssdConfigured };
}

type ExperimentLifecycleDeps = Pick<ShellState, "append">
  & Pick<MonitoringState, "experimentPending" | "setExperimentRequestBusy" | "setReadinessMessage" | "setStartNotice"
    | "setExpStatus" | "finishedRun" | "loggedRun" | "expStatus" | "setRunAccounting">;

// Start / Stop Experiment, and the outcome line logged once per finished run.
export function useExperimentLifecycle({
  append, experimentPending, setExperimentRequestBusy, setReadinessMessage, setStartNotice, setExpStatus, finishedRun,
  loggedRun, expStatus, setRunAccounting,
}: ExperimentLifecycleDeps) {
  // ---- Experiment lifecycle (backend-owned, BE-4) ----

  const onStartExperiment = useCallback(async () => {
    if (experimentPending.current) return;
    experimentPending.current = true; setExperimentRequestBusy(true); setReadinessMessage(""); setStartNotice("");
    try {
      const outcome = await startExperiment({
        fetchRingStatus: () => bridge.fetchRingStatus(),
        confirm,
        pickFile: () => save({ title: "Save Experiment Data", filters: H5_FILTER, defaultPath: "experiment.h5" }),
        fetchReadiness: (path) => bridge.fetchExperimentReadiness(path),
        start: (path, acknowledgeDiscardRing) => bridge.experimentStart(path, acknowledgeDiscardRing),
      });
      if (outcome.kind === "not-ready") { setReadinessMessage(outcome.message); return; }
      if (outcome.kind === "refused") {setReadinessMessage(`${outcome.path}: ${outcome.message}`); setExpStatus(await bridge.fetchExperimentStatus()); return append(`experiment start failed: ${outcome.message}`); }
      if (outcome.kind === "started") {
        // The backend's path: a relative one is resolved under the data directory (#762).
        const status = await bridge.fetchExperimentStatus();
        append(`experiment started → ${status?.output_path || outcome.path}`);
        if (outcome.notice) { setStartNotice(outcome.notice); append(outcome.notice); }
        setExpStatus(status);
      }
    } catch (e) {
      append(`experiment start error: ${e}`);
    } finally { experimentPending.current = false; setExperimentRequestBusy(false); }
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

  useEffect(() => {
    if (!finishedRun || finishedRun === loggedRun.current) return;
    loggedRun.current = finishedRun;
    let live = true;
    const status = expStatus;
    void bridge.fetchRunAccounting("last_run").catch(() => null).then((acc) => {
      if (!live) return;
      setRunAccounting(acc);
      const outcome = describeRunOutcome(status, acc);
      if (outcome) append(outcome.headline);
    });
    return () => { live = false; };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [finishedRun]);
  return { onStartExperiment, onStopExperiment };
}

type ExperimentModelDeps = Pick<ShellState, "ready" | "instrumentModes"> & Pick<ConnectState, "running">
  & Pick<MonitoringState, "expActive" | "experimentRequestBusy"> & CoresModel & { runModeBlock: string | undefined };

// Render-time value: why Start Experiment is disabled (undefined when it is enabled).
export function experimentModel({
  ready, instrumentModes, running, expActive, experimentRequestBusy, cores, runModeBlock,
}: ExperimentModelDeps) {
  const startExperimentReason = !ready || !cores.initialized
    ? "Backend is not initialized"
    : instrumentModes && runModeBlock
      ? runModeBlock
      : !instrumentModes && !running
        ? "Camera must be running before starting an experiment"
        : expActive
          ? "Experiment is already running"
          : experimentRequestBusy ? "Experiment start request pending" : undefined;
  return { startExperimentReason };
}
