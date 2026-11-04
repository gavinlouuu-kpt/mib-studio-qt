// Shell (epic #862): capture and experiment recovery, re-analysis and export status banners.
// Moved from App.tsx without change (#869 wave 0).
import { EXPERIMENT_STATES } from "../../bridgeContract";
import { CaptureRecovery } from "../../components/CaptureRecovery";
import { ExperimentRecovery } from "../../components/ExperimentRecovery";
import { ViewerGate } from "../../components/ViewerGate";
import { ExportStatus } from "../../exportControls";
import { ReanalysisStatus } from "../../reanalysisControls";
import { useStudio } from "../../studio/StudioContext";

export function RecoveryBanners() {
  const {
    ready, expActive, expStatus, expState, recording, cameraScript, onStartCamera, setTab, setExpStatus, caps, reanalysis,
    reviewExport,
  } = useStudio();
  return (
    <ViewerGate>
    <CaptureRecovery ready={ready} blocked={expActive || !!expStatus?.flushing || (expState === EXPERIMENT_STATES.Failed && !expStatus?.terminal) || recording || cameraScript.busy} onRetry={onStartCamera} onConfigure={() => setTab("connect")} />
    <ExperimentRecovery ready={ready} status={expStatus} onStatus={setExpStatus} />
    {caps.reanalysis && <ReanalysisStatus model={reanalysis}/>}
    <ExportStatus model={reviewExport} />
    </ViewerGate>
  );
}
