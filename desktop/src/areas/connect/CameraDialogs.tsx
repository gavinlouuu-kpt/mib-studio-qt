// Connect and Overview (epic #863): the modal dialogs of the Connect stage. "No camera found" after the startup scan
// (Qt MainWindow::onNoCamerasFound) and the "Camera" / "Connect Device" warning of a refused camera command
// (Qt QMessageBox::warning). Both use the app's modal pattern (styles/modal.css).
import { ViewerGate } from "../../components/ViewerGate";
import { useStudio } from "../../studio/StudioContext";

export function CameraDialogs() {
  const { noCameraDialog, setNoCameraDialog, onTryCameraAgain, cameraNotice, setCameraNotice } = useStudio();
  return (
    <>
      {noCameraDialog && (
        <div className="modal-backdrop">
          <div className="modal" role="alertdialog" aria-label="No camera found" aria-describedby="no-camera-text">
            <h3>No camera found</h3>
            <p id="no-camera-text">
              No camera was found.
              <br /><br />
              Please check that the camera is powered on and that the camera LED is green (ready).
              <br />
              Then try connecting again.
            </p>
            <div className="actions">
              <ViewerGate>
                <button className="btn" onClick={() => void onTryCameraAgain()}>Try again</button>
              </ViewerGate>
              <button className="btn" autoFocus onClick={() => setNoCameraDialog(false)}>OK</button>
            </div>
          </div>
        </div>
      )}
      {cameraNotice && (
        <div className="modal-backdrop" onClick={() => setCameraNotice(null)}>
          <div className="modal" role="alertdialog" aria-label={cameraNotice.title} onClick={(e) => e.stopPropagation()}>
            <h3>{cameraNotice.title}</h3>
            <p>{cameraNotice.message}</p>
            <div className="actions">
              <button className="btn" autoFocus onClick={() => setCameraNotice(null)}>OK</button>
            </div>
          </div>
        </div>
      )}
    </>
  );
}
