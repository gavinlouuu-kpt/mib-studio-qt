// Connect and Overview (epic #863): Start / Stop Camera in the tab header. Moved from App.tsx without change (#869 wave 0).
import { ViewerGate } from "../../components/ViewerGate";
import { useStudio } from "../../studio/StudioContext";

export function CameraActions() {
  const { onStartCamera, startCameraReason, onStopCamera, running, expActive } = useStudio();
  return (
    <div className="camera-actions"><ViewerGate>
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
    </ViewerGate></div>
  );
}
