// Connect and Overview (epic #863): Start / Stop Camera in the tab header. Moved from App.tsx without change (#869 wave 0).
// Stop Camera's refusal reasons (an experiment, a save in progress, raw recording) come from monitoring/runStatus (#866 MON-012).
import { ViewerGate } from "../../components/ViewerGate";
import { useStudio } from "../../studio/StudioContext";

export function CameraActions() {
  const { onStartCamera, startCameraReason, onStopCamera, running, stopCameraBlock } = useStudio();
  return (
    <div className="camera-actions"><ViewerGate>
      <button onClick={onStartCamera} disabled={!!startCameraReason} title={startCameraReason}>
        Start Camera
      </button>
      <button
        onClick={onStopCamera}
        disabled={!running || !!stopCameraBlock}
        title={!running ? "Camera is not running" : stopCameraBlock}
      >
        Stop Camera
      </button>
    </ViewerGate></div>
  );
}
