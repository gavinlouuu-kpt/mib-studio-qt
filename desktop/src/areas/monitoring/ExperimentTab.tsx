// Experiment start/stop and Monitoring (epic #866): the Experiment stage: its sub-tabs, Start / Stop
// Experiment, readiness and outcome notices, the SSD strip, Files and Recordings. The Preview sub-tab
// is areas/preview, Monitoring is MonitoringPanel. Moved from App.tsx without change (#869 wave 0).
import { EXPERIMENT_STATES } from "../../bridgeContract";
import { FilesPanel } from "../../components/FilesPanel";
import { RunOutcomeNotice } from "../../components/RunOutcomeNotice";
import { RunRateControl } from "../../components/RunRateControl";
import { RunWindowNotice } from "../../components/RunWindowNotice";
import { SsdRunsPanel } from "../../components/SsdRunsPanel";
import { SsdStrip } from "../../components/SsdStrip";
import { ViewerGate } from "../../components/ViewerGate";
import { downloadUrl } from "../../filesView";
import { isRemote } from "../../transport";
import { serverOrigin, tokenFromLocation } from "../../transport/auth";
import { useStudio } from "../../studio/StudioContext";
import { CameraStageNotice } from "../connect/CameraStageNotice";
import { PreviewPanel } from "../preview/PreviewPanel";
import { MonitoringPanel } from "./MonitoringPanel";

export function ExperimentTab() {
  const {
    ssdConfigured, ssd, expTab, setExpTab, roiFields, onStartExperiment, startExperimentReason, onStopExperiment, expState,
    instrumentModes, runFpsStatus, cameraWin, modeError, setTab, setModeRetry, readinessMessage, expActive, runOutcome,
    expStatus, startNotice, tab, instrument,
  } = useStudio();
  return (
    <>
      {isRemote && ssdConfigured && <SsdStrip status={ssd} />}
      <div className="toolbar">
        <div className="subtabs" role="tablist" aria-label="Experiment views">
          <button className={expTab === "preview" ? "active" : ""} onClick={() => setExpTab("preview")}>
            Preview
          </button>
          {isRemote && <button className={expTab === "files" ? "active" : ""} onClick={() => setExpTab("files")}>
            Files
          </button>}
          {isRemote && ssdConfigured && <button className={expTab === "recordings" ? "active" : ""} onClick={() => setExpTab("recordings")}>
            Recordings
          </button>}
          <button className={expTab === "monitoring" ? "active" : ""} onClick={() => setExpTab("monitoring")}>
            Monitoring
          </button>
        </div>
        <div className="right">
          <span className="mono">
            ROI: {Number(roiFields.w) > 0 ? `${roiFields.w} × ${roiFields.h} @ (${roiFields.x}, ${roiFields.y})` : "full frame"}
          </span>
          <ViewerGate>
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
          </ViewerGate>
        </div>
      </div>

      {instrumentModes && <ViewerGate><RunRateControl status={runFpsStatus} /></ViewerGate>}
      {instrumentModes && <RunWindowNotice tab="experiment" windowPlaced={cameraWin.placed} modeError={modeError}
        onPlaceWindow={() => setTab("overview")} onRetry={() => setModeRetry((n) => n + 1)} />}
      <CameraStageNotice stage="experiment" />
      {readinessMessage && <p role="alert">Experiment readiness: {readinessMessage}</p>}
      {!expActive && <RunOutcomeNotice outcome={runOutcome} />}
      {/* The finished run's file can be fetched from the browser (G3): the instrument keeps it on its own disk. */}
      {isRemote && !expActive && expStatus?.valid && expStatus.terminal && expStatus.finalization_ok && !expStatus.cancelled && expStatus.output_path && (
        <p className="mono"><a href={downloadUrl(serverOrigin(), expStatus.output_path, tokenFromLocation())} download>
          Download {expStatus.output_path.split("/").pop()}
        </a></p>
      )}
      {startNotice && <p role="status" className="start-notice">{startNotice}</p>}

      {expTab === "preview" && <PreviewPanel />}

      {expTab === "recordings" && isRemote && ssdConfigured && <SsdRunsPanel status={ssd} active={tab === "experiment" && expTab === "recordings"} />}
      {expTab === "files" && isRemote && <FilesPanel ramWarning={instrument?.storage?.warning || undefined} />}
      {expTab === "monitoring" && <MonitoringPanel />}
    </>
  );
}
