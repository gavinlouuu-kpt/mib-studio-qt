// Connect and Overview (epic #863, CONNECT-047): the Overview's "ROI Overlay: On/Off" toggle (Qt OverviewTab
// roiOverlayBtn). The rectangle on the live image is the camera window; Qt starts with the overlay off and switches
// it on when a camera is connected (MainWindow: ConnectTab::connected -> setRoiOverlayVisible(true)). While it is off
// the box is not drawn and cannot be dragged (SimpleImageCanvas draws and grabs it only when visible).
import type { CameraSelection } from "../../bridge";

/** Qt's button texts. */
export const roiOverlayLabel = (on: boolean): string => (on ? "ROI Overlay: On" : "ROI Overlay: Off");
export const ROI_OVERLAY_TITLE = "Toggle ROI overlay rectangle";

/** Which camera is connected: "" until a valid, configured camera is selected; a new key for another camera or
 *  another source. Polled selection snapshots of the same camera give the same key, so a poll never switches the
 *  overlay back on after the operator turned it off. */
export function connectedCameraKey(selection: CameraSelection | null | undefined): string {
  if (!selection?.valid || !selection.configured) return "";
  return [selection.mode, selection.interface_index, selection.device_index, selection.mindvision_index, selection.label,
    selection.mindvision_config_path, selection.mock_frame_dir].join("|");
}

/** Switch the overlay on: a camera is connected that was not before (Qt's `connected` signal). Nothing happens when the
 *  camera disconnected or the same camera is polled again, so the operator's choice stays. */
export function overlaySwitchesOn(previousKey: string, key: string): boolean {
  return key !== "" && key !== previousKey;
}
