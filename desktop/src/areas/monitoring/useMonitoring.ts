// Experiment start/stop and Monitoring (epic #866): the backend-owned experiment lifecycle, the
// last run's outcome, the Monitoring snapshot polling, the standing Start readiness and the SATA SSD
// record store status. Moved from App.tsx (#869 wave 0); studio/useStudioController.ts calls these
// hooks in App.tsx's original effect order. The Qt-parity behaviour (epic #866, PR D) is in the pure
// modules beside this file: startReadiness, runStatus, runFinish, statsReadouts, pollHealth, clearBuffer.
import { useCallback, useEffect, useMemo, useReducer, useRef, useState } from "react";
import { bridge, type ExperimentStatus, type MonitoringSnapshot, type RunAccounting, type TriggerStatus } from "../../bridge";
import { EXPERIMENT_STATES } from "../../bridgeContract";
import { startExperiment, type StartReadiness } from "../../experimentStart";
import { describeRunOutcome, runKey } from "../../runOutcome";
import type { SsdStatus } from "../../ssdView";
import { isRemote } from "../../transport";
import { confirm, message, save } from "../../transport/dialogs";
import { initialTuneState, tuneReducer, type TuneFieldId } from "./tuneDraft";
import type { ConfigState, CoresModel } from "../config/useConfig";
import type { ConnectState } from "../connect/useConnect";
import type { HardwareState } from "../hardware/useHardware";
import type { PreviewState } from "../preview/usePreview";
import type { ReviewState } from "../review/useReview";
import type { ShellState } from "../shell/useShell";
import { MONITORING_FETCH_ROWS, sameSnapshot } from "./monitoringChartModel";
import {
  DEFAULT_MONITORING_SETTINGS, ringRatioRangeOfConfig, withHistogramXRange, type MonitoringSettings,
} from "./monitoringSettings";
import { POLL_HEALTHY, pollHealthAfter, pollNotice, type PollHealth } from "./pollHealth";
import { accountingDialog } from "./runFinish";
import { cameraStopBlock, deriveRunPresentation, recordBlock, runProgressText } from "./runStatus";
import { sameReadiness, seriesForcesInline, summarizeReadiness } from "./startReadiness";

/** MON-009: Qt's "Save Experiment Data" filter, "HDF5 Files (*.h5 *.hdf5);;All Files (*)". Review shares H5_FILTER
 *  (studio/constants.ts), which stays .h5 only. */
export const EXPERIMENT_FILE_FILTER = [
  { name: "HDF5 Files", extensions: ["h5", "hdf5"] },
  { name: "All Files", extensions: ["*"] },
];

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
  // Run state for the operator (MON-022/025): Start has left for the backend / Stop was pressed and the backend has not said so yet.
  const [startInFlight, setStartInFlight] = useState(false);
  const [stopRequested, setStopRequested] = useState(false);
  // The finished run the operator moved past (the camera started or stopped after it): Qt drops its Complete state then.
  const [runDismissed, setRunDismissed] = useState("");
  // The standing readiness with no destination (MON-028), polled while the Experiment stage is shown.
  const [startReadiness, setStartReadiness] = useState<StartReadiness | null>(null);
  const [readinessHealth, setReadinessHealth] = useState<PollHealth>(POLL_HEALTHY);
  // Failures the Monitoring polls used to swallow (App.tsx:533, 542-544, 1031 at 5f906ac2).
  const [monitoringIssue, setMonitoringIssue] = useState("");
  const [snapshotHealth, setSnapshotHealth] = useState<PollHealth>(POLL_HEALTHY);
  const [triggerHealth, setTriggerHealth] = useState<PollHealth>(POLL_HEALTHY);
  const [accountingError, setAccountingError] = useState("");
  // A new value each time the processing configuration was re-read with a background set: the side panel's thumbnail refetches on it (MON-102).
  const [backgroundRevision, setBackgroundRevision] = useState(0);
  // Whoever shows the monitoring buffer's derived state (KDE, the selection) registers here: called after Clear Buffer succeeded (MON-029).
  const monitoringClearedListeners = useRef(new Set<() => void>());
  /** The hook point: `const off = onMonitoringCleared(() => resetKde())` (returns the unsubscribe); called after Clear Buffer succeeded. */
  const onMonitoringCleared = useCallback((listener: () => void) => {
    monitoringClearedListeners.current.add(listener);
    return () => { monitoringClearedListeners.current.delete(listener); };
  }, []);
  const notifyMonitoringCleared = useCallback(() => { monitoringClearedListeners.current.forEach((listener) => listener()); }, []);
  return {
    tune, dispatchTune, tuneDraft, setTuneField,
    expTab, setExpTab, expStatus, setExpStatus, monSnapshot, setMonSnapshot, monSettings, setMonSettings,
    showMonitoringSettings, setShowMonitoringSettings,
    experimentPending, experimentRequestBusy, setExperimentRequestBusy, readinessMessage, setReadinessMessage,
    startNotice, setStartNotice, expState, runAccounting, setRunAccounting, runOutcome, finishedRun, loggedRun,
    elapsedWallSeconds, expActive,
    startInFlight, setStartInFlight, stopRequested, setStopRequested, runDismissed, setRunDismissed,
    startReadiness, setStartReadiness, readinessHealth, setReadinessHealth, monitoringIssue, setMonitoringIssue,
    snapshotHealth, setSnapshotHealth, triggerHealth, setTriggerHealth, accountingError, setAccountingError,
    backgroundRevision, setBackgroundRevision, onMonitoringCleared, notifyMonitoringCleared,
  };
}
export type MonitoringState = ReturnType<typeof useMonitoringState>;

type MonitoringPollingDeps = Pick<ShellState, "ready" | "tab" | "append" | "caps">
  & Pick<MonitoringState, "expTab" | "setMonSnapshot" | "setMonitoringIssue" | "setSnapshotHealth" | "setTriggerHealth">
  & Pick<HardwareState, "setTrigStatus">;

/** The one visibility predicate of Monitoring (BE-5): the Experiment stage with its Monitoring
 *  sub-tab shown. The snapshot polling and the live cell strips (CellStrips.tsx) both gate on it. */
export const isMonitoringVisible = (tab: string, expTab: string): boolean => tab === "experiment" && expTab === "monitoring";

// A thrown value that is never undefined (pollHealthAfter reads undefined as "the poll succeeded").
const thrown = (e: unknown): unknown => (e === undefined || e === null ? "unknown error" : e);

export function useMonitoringPolling({
  ready, tab, expTab, append, caps, setMonSnapshot, setTrigStatus, setMonitoringIssue, setSnapshotHealth, setTriggerHealth,
}: MonitoringPollingDeps) {
  // Monitoring is visibility-gated (BE-5): accumulation and its per-frame
  // image clones run only while the Monitoring view is actually shown.
  const monitoringVisible = isMonitoringVisible(tab, expTab);
  // The platform info arrives after the first render: read the capability when a poll finishes, not when the effect starts.
  const hasTrigger = useRef(caps.trigger);
  hasTrigger.current = caps.trigger;
  useEffect(() => {
    if (!ready) return;
    let disposed = false;
    // A refused switch used to vanish: Monitoring then showed an empty buffer with no word why (App.tsx:533 at 5f906ac2).
    const verb = monitoringVisible ? "enable" : "disable";
    const failed = (why: string) => {
      if (disposed) return;
      append(`monitoring could not ${verb} the backend's accumulation: ${why}`);
      if (monitoringVisible) setMonitoringIssue(`Monitoring could not be switched on in the backend (${why}): rows and charts may stay empty.`);
    };
    bridge.monitoringSetActive(monitoringVisible)
      .then((res) => { if (res.ok) { if (!disposed) setMonitoringIssue(""); } else failed(res.message || "refused"); })
      .catch((e) => failed(String(e)));
    if (!monitoringVisible) return () => { disposed = true; };
    setSnapshotHealth(POLL_HEALTHY); setTriggerHealth(POLL_HEALTHY);
    let polling = false;
    const id = window.setInterval(async () => {
      if (polling) return; polling = true;
      try {
        // Each read counts on its own: a trigger service that stops answering must not stop the buffer table, and the
        // other way round. One lost tick is a blip; consecutive failures raise the notice in the Monitoring panel.
        let snapshot: MonitoringSnapshot | null = null, snapshotError: unknown;
        let trigger: TriggerStatus | null = null, triggerError: unknown;
        // The plotted window (1000 valid rows, MON-036) plus the latest-rows table, not a fixed 200 rows.
        try { snapshot = await bridge.fetchMonitoringSnapshot(MONITORING_FETCH_ROWS); } catch (e) { snapshotError = thrown(e); }
        try { trigger = await bridge.fetchTriggerStatus(); } catch (e) { triggerError = thrown(e); }
        if (disposed) return;
        // A poll that finds nothing new keeps the previous snapshot: no re-render, no redraw.
        if (snapshot) { const fresh = snapshot; setMonSnapshot((previous) => (sameSnapshot(previous, fresh) ? previous : fresh)); }
        if (trigger) setTrigStatus(trigger);
        setSnapshotHealth((h) => pollHealthAfter(h, snapshotError));
        // A trigger service the instrument does not have is not an error.
        setTriggerHealth((h) => pollHealthAfter(h, hasTrigger.current ? triggerError : undefined));
      } finally { polling = false; }
    }, 500);
    return () => { disposed = true; window.clearInterval(id); };
    // eslint-disable-next-line react-hooks/exhaustive-deps
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

type StartReadinessDeps = Pick<ShellState, "ready" | "tab"> & Pick<MonitoringState, "expActive" | "setStartReadiness" | "setReadinessHealth">;

// The standing readiness (MON-028): what would block Start, and what only warns, read with no destination while the
// Experiment stage is shown and no run holds its plan. Qt does the same every second (MainWindow::refreshRecordingCapacity).
// The read is the backend's own evaluation, not a copy of its rules; one at a time, every 2 s, no state set when nothing changed.
export function useStartReadiness({ ready, tab, expActive, setStartReadiness, setReadinessHealth }: StartReadinessDeps) {
  const visible = tab === "experiment" && !expActive;
  useEffect(() => {
    if (!ready || !visible) return;
    let disposed = false, polling = false;
    const poll = async () => {
      if (polling) return; polling = true;
      try {
        const readiness = await bridge.fetchExperimentReadiness("");
        if (disposed) return;
        setStartReadiness((prev) => (sameReadiness(prev, readiness) ? prev : readiness));
        setReadinessHealth((h) => pollHealthAfter(h));
      } catch (e) {
        if (!disposed) setReadinessHealth((h) => pollHealthAfter(h, thrown(e)));
      } finally { polling = false; }
    };
    void poll();
    const id = window.setInterval(() => void poll(), 2000);
    return () => { disposed = true; window.clearInterval(id); };
  }, [ready, visible]);
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

type ExperimentLifecycleDeps = Pick<ShellState, "append" | "hostProcessing"> & Pick<ConnectState, "running">
  & Pick<MonitoringState, "experimentPending" | "setExperimentRequestBusy" | "setReadinessMessage" | "setStartNotice"
    | "setExpStatus" | "finishedRun" | "loggedRun" | "expStatus" | "setRunAccounting" | "setStartInFlight" | "setStopRequested"
    | "setRunDismissed" | "setAccountingError">;

// Start / Stop Experiment, and the outcome line logged once per finished run.
export function useExperimentLifecycle({
  append, hostProcessing, running, experimentPending, setExperimentRequestBusy, setReadinessMessage, setStartNotice, setExpStatus,
  finishedRun, loggedRun, expStatus, setRunAccounting, setStartInFlight, setStopRequested, setRunDismissed, setAccountingError,
}: ExperimentLifecycleDeps) {
  // ---- Experiment lifecycle (backend-owned, BE-4) ----

  const onStartExperiment = useCallback(async () => {
    if (experimentPending.current) return;
    experimentPending.current = true; setExperimentRequestBusy(true); setReadinessMessage(""); setStartNotice("");
    try {
      const outcome = await startExperiment({
        fetchRingStatus: () => bridge.fetchRingStatus(),
        confirm,
        // No default name (Qt: ""): the desktop dialog opens where the operator last saved. A browser has no file dialog, so its
        // prompt keeps a suggested name under the instrument's data directory (#651 G8).
        pickFile: () => save({ title: "Save Experiment Data", filters: EXPERIMENT_FILE_FILTER, defaultPath: isRemote ? "experiment.h5" : undefined }),
        fetchReadiness: (path) => bridge.fetchExperimentReadiness(path),
        start: async (path, acknowledgeDiscardRing) => {
          setStartInFlight(true);
          try {
            const res = await bridge.experimentStart(path, acknowledgeDiscardRing);
            return { ok: res.ok, message: res.message, outcome: res.start_outcome };
          } finally { setStartInFlight(false); }
        },
        inform: ({ kind, title, text }) => message(text, { title, kind }),
        // MON-008: a multi-image series runs inline; only the host pipeline has the realtime mode.
        needsInlineForSeries: hostProcessing
          ? async () => { const doc = await bridge.fetchProcessingConfigJson(); return doc.valid && seriesForcesInline(doc.json); }
          : undefined,
      });
      if (outcome.kind === "not-ready") { setReadinessMessage(outcome.message); append(`experiment not ready: ${outcome.message}`); return; }
      if (outcome.kind === "refused") {
        setReadinessMessage(`${outcome.path}: ${outcome.summary}`);
        setExpStatus(await bridge.fetchExperimentStatus());
        return append(`experiment start failed: ${outcome.summary}`);
      }
      if (outcome.kind === "started") {
        // The backend's path: a relative one is resolved under the data directory (#762).
        const status = await bridge.fetchExperimentStatus();
        append(`experiment started → ${status?.output_path || outcome.path}`);
        if (outcome.notice) { setStartNotice(outcome.notice); append(outcome.notice.replace(/\n/g, "; ")); }
        setExpStatus(status);
      }
    } catch (e) {
      append(`experiment start error: ${e}`);
    } finally { experimentPending.current = false; setExperimentRequestBusy(false); }
  }, [append, hostProcessing]);

  const onStopExperiment = useCallback(async () => {
    // "Stopping experiment…" until the backend reports it is saving (MON-025); the run state widget reads this flag.
    setStopRequested(true);
    try {
      const res = await bridge.experimentStop();
      if (!res.ok) append(`experiment stop failed: ${res.message}`);
      else append("Stopping experiment…");
      // Also after a refusal: a run the backend already finalized (a fatal save error) is mirrored by the fresh status (Qt NotActive).
      setExpStatus(await bridge.fetchExperimentStatus());
    } catch (e) {
      append(`experiment stop error: ${e}`);
    } finally { setStopRequested(false); }
  }, [append]);

  // The run this client saw go through Starting/Active/Stopping: only such a run ends with the loss modal (a status recovered
  // after a reload belongs to a run the operator already dealt with).
  const observedRun = useRef("");
  useEffect(() => {
    if (expStatus?.valid && !expStatus.terminal && expStatus.state !== EXPERIMENT_STATES.Idle) observedRun.current = expStatus.start_generation;
  }, [expStatus]);

  // Qt drops "Complete" from the run state widget once the camera state changes after the run.
  const lastRunning = useRef(running);
  useEffect(() => {
    if (lastRunning.current === running) return;
    lastRunning.current = running;
    if (finishedRun) setRunDismissed(finishedRun);
  }, [running, finishedRun]);

  useEffect(() => {
    if (!finishedRun || finishedRun === loggedRun.current) return;
    loggedRun.current = finishedRun;
    let live = true;
    const status = expStatus;
    const observed = !!status && observedRun.current === status.start_generation;
    if (!observed) setRunDismissed(finishedRun);
    // A failed read used to vanish (App.tsx:1031 at 5f906ac2): the loss counts then came from the status alone with no word why.
    void bridge.fetchRunAccounting("last_run").then(
      (acc) => ({ acc, error: !acc.available && acc.error ? acc.error : "" }),
      (e) => ({ acc: null as RunAccounting | null, error: String(e) }),
    ).then(({ acc, error }) => {
      if (!live) return;
      setRunAccounting(acc);
      if (error) {
        setAccountingError(`The run's frame accounting could not be read (${error}); the counts below come from the saved status only.`);
        append(`run accounting unavailable: ${error}`);
      } else setAccountingError("");
      const outcome = describeRunOutcome(status, acc);
      if (outcome) append(outcome.headline);
      const presentation = deriveRunPresentation({ cameraRunning: running, status });
      const saved = runProgressText(presentation, status);
      if (saved && observed) append(saved);
      // The modal waits for the file to close (this status is terminal): a dialog before it held the HDF5 open for 108 s on the Qt bench.
      const dialog = observed ? accountingDialog(status, acc) : null;
      if (dialog) void message(dialog.text, { title: dialog.title, kind: dialog.kind });
    });
    return () => { live = false; };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [finishedRun]);
  return { onStartExperiment, onStopExperiment };
}

type ExperimentModelDeps = Pick<ShellState, "ready" | "instrumentModes"> & Pick<ConnectState, "running">
  & Pick<PreviewState, "recording"> & Pick<ReviewState, "reviewMeta">
  & Pick<MonitoringState, "expActive" | "expState" | "expStatus" | "experimentRequestBusy" | "startInFlight" | "stopRequested"
    | "runDismissed" | "startReadiness" | "readinessHealth" | "monitoringIssue" | "snapshotHealth" | "triggerHealth" | "accountingError">
  & Pick<ShellState, "caps"> & CoresModel & { runModeBlock: string | undefined };

// Render-time values: why Start Experiment is disabled (undefined when it is enabled), the run state and what it locks,
// the standing readiness, and the Monitoring failures to tell the operator about.
export function experimentModel({
  ready, instrumentModes, running, expActive, expState, expStatus, experimentRequestBusy, cores, runModeBlock, recording, reviewMeta,
  startInFlight, stopRequested, runDismissed, startReadiness, readinessHealth, monitoringIssue, snapshotHealth, triggerHealth, accountingError, caps,
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
  // MON-022: the Qt run state (glyph + text); `runLocked` is Qt's experimentActive_ (MON-013: Overview and Review are shut for the whole run).
  const runPresentation = deriveRunPresentation({ cameraRunning: running, status: expStatus, startInFlight, stopRequested, dismissedRun: runDismissed });
  const runLocked = runPresentation.locked;
  const runProgress = runProgressText(runPresentation, expStatus);
  // MON-012 and MON-014: the reasons Stop Camera and Raw Record are refused (the controls are in connect/ and preview/).
  const stopCameraBlock = cameraStopBlock({ expActive: runLocked, flushing: !!expStatus?.flushing, recording });
  const recordReason = recordBlock({ recording, experimentIdle: expState === EXPERIMENT_STATES.Idle, hdf5Open: !!reviewMeta?.file_open });
  // MON-028: what would block Start, and what only warns.
  const readinessSummary = startReadiness?.valid ? summarizeReadiness(startReadiness.gates) : null;
  const readinessNotice = pollNotice("The Start readiness check", readinessHealth);
  // The Monitoring panel's failures: a refused switch-on, polls that keep failing, a trigger service that stops answering.
  const monitoringNotices = [monitoringIssue, pollNotice("Monitoring data", snapshotHealth), caps.trigger ? pollNotice("Trigger status", triggerHealth) : ""].filter(Boolean);
  return {
    startExperimentReason, runPresentation, runLocked, runProgress, stopCameraBlock, recordReason, readinessSummary, readinessNotice,
    monitoringNotices, accountingNotice: accountingError,
  };
}
