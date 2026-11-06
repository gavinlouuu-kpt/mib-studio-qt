// Review (epic #867, REVIEW-007): Qt locks the Review tab while an experiment is active
// (MainWindow.cpp updateTabStates disables it; a click shows this warning and returns to the
// Experiment tab). MIB Studio's stage tabs live in the shell area, so the Review stage itself
// carries the lock: the file controls are disabled and say why.

/** Qt's warning (MainWindow.cpp, tab change guard), title "Review Tab". */
export const REVIEW_LOCKED_EXPERIMENT = "Review tab is disabled while an experiment is active. Please stop the experiment first.";
/** The backend's own refusal of a load while frames are being recorded (BackendFacade::handleRecordingLoadCommand). */
export const REVIEW_LOCKED_RECORDING = "Stop raw recording before loading review data";

/** Why the Review stage cannot open a file now, or null. An experiment wins over a raw recording. */
export function reviewLockReason(expActive: boolean, recording: boolean): string | null {
  if (expActive) return REVIEW_LOCKED_EXPERIMENT;
  return recording ? REVIEW_LOCKED_RECORDING : null;
}
