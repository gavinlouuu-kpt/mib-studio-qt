// Connect and Overview (epic #863): Start / Stop Camera in the tab header, with the camera's phase beside them (Qt's
// CameraController: "Camera starting…", "Camera running", "Camera stopping…", "Camera failed", "Camera not configured").
import { ViewerGate } from "../../components/ViewerGate";
import { useStudio } from "../../studio/StudioContext";
import { stopCameraBlockReason } from "./connectFlow";

export function CameraActions() {
  const { onStartCamera, startCameraReason, onStopCamera, running, expState, cameraCommand, cameraPhase } = useStudio();
  const stopReason = stopCameraBlockReason({ running, expState, commandInFlight: cameraCommand !== null });
  return (
    <div className="camera-actions"><ViewerGate>
      <button onClick={onStartCamera} disabled={!!startCameraReason} title={startCameraReason}>
        Start Camera
      </button>
      <button
        onClick={onStopCamera}
        disabled={!!stopReason}
        title={stopReason}
      >
        Stop Camera
      </button>
    </ViewerGate>
    <span
      className={`camera-phase ${cameraPhase.phase}`}
      role="status"
      aria-live="polite"
      title={cameraPhase.failureMessage || undefined}
    >
      {cameraPhase.text}{cameraPhase.failureMessage ? `: ${cameraPhase.failureMessage}` : ""}
    </span></div>
  );
}
