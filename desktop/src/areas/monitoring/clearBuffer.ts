// Clear Buffer (epic #866: MON-029). Qt asks Yes/No, default No, before it clears the monitoring buffer
// (ExperimentMonitoringTab::onClearBuffer); only on Yes does it clear the frames, and then the KDE state and the
// selection. The backend clears the frame ring here; whoever owns the KDE state or the selection (later PRs of
// the stream) registers with `onMonitoringCleared` in useMonitoring and is called after a successful clear.
// Pure flow: the dialog, the clear and the log are injected.

export const CLEAR_BUFFER_QUESTION = "Are you sure you want to clear the monitoring buffer? This will remove all accumulated frames.";

export interface ClearBufferDeps {
  /** A Yes / No question; the plugin's ask. The dialog cannot preselect No: closing it or pressing No keeps the buffer. */
  ask: (message: string, options: { title: string; kind: "warning" }) => Promise<boolean>;
  clear: () => Promise<{ ok: boolean; message: string }>;
  /** After a successful clear: the monitoring-cleared listeners and the local snapshot reset. */
  onCleared: () => void;
  append: (line: string) => void;
}

export type ClearBufferResult = "declined" | "cleared" | "failed";

export async function clearMonitoringBuffer({ ask, clear, onCleared, append }: ClearBufferDeps): Promise<ClearBufferResult> {
  let yes = false;
  try {
    yes = await ask(CLEAR_BUFFER_QUESTION, { title: "Clear Buffer", kind: "warning" });
  } catch (e) {
    append(`clear buffer: the question could not be shown (${e}); nothing was cleared`);
    return "declined";
  }
  if (!yes) return "declined";
  try {
    const res = await clear();
    if (!res.ok) { append(`clear failed: ${res.message}`); return "failed"; }
  } catch (e) {
    append(`clear error: ${e}`);
    return "failed";
  }
  onCleared();
  append("monitoring buffers cleared");
  return "cleared";
}
