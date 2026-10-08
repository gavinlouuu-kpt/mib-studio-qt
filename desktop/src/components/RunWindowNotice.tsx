import type { CameraModeError } from "../cameraWindow";

// PZ7035 camera-mode problems the operator has to act on (#501): the Run window has not been
// placed yet, or the backend refused the Align/Run switch. Shown in the tab it concerns, in the
// way preflight shows a failed check; the Start button gives the same reason.
export function RunWindowNotice({ tab, windowPlaced, modeError, onPlaceWindow, onRetry }: {
  tab: "overview" | "experiment";
  windowPlaced: boolean;
  modeError: CameraModeError | null;
  onPlaceWindow: () => void;
  onRetry: () => void;
}) {
  if (tab === "experiment" && !windowPlaced) {
    return <div role="alert" className="mode-notice">
      <span>Place the 512×96 run window in Camera &amp; Alignment: the experiment cannot start until it is set.</span>
      <button onClick={onPlaceWindow}>Go to Camera &amp; Alignment</button>
    </div>;
  }
  const failed = modeError && modeError.mode === (tab === "experiment" ? "run" : "align") ? modeError : null;
  if (!failed) return null;
  return <div role="alert" className="mode-notice">
    <span>The camera did not switch to {failed.mode === "run" ? "Run" : "Align"}: {failed.message}</span>
    <button onClick={onRetry}>Retry</button>
  </div>;
}
