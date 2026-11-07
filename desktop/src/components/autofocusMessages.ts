// Autofocus runtime messages (HW-033, epic #868): failures the backend reports as events while the operator is not looking at a
// command result (a voltage change the controller refused, the safe shutdown voltage that was not applied, why a connect
// failed). The shell routes BackendError events of command type Autofocus here; the nanopositioner panel shows them as its status
// line, and the shell's own handler has already written them to the event log. Qt shows the same messages in the tab's status
// label (NanopositionerTab::setNanopositionerStatus).
export interface AutofocusMessage { text: string; at: number }
const MAX_KEPT = 5;
const listeners = new Set<(message: AutofocusMessage) => void>();
let recent: AutofocusMessage[] = [];

export function publishAutofocusMessage(text: string, now: number = Date.now()): void {
  const message = {text, at: now};
  recent = [...recent.slice(-(MAX_KEPT - 1)), message];
  for (const listener of listeners) listener(message);
}
/** The newest messages, oldest first: a panel that mounts after an event still shows it. */
export const recentAutofocusMessages = (): AutofocusMessage[] => recent;
export function subscribeAutofocusMessages(listener: (message: AutofocusMessage) => void): () => void {
  listeners.add(listener);
  return () => { listeners.delete(listener); };
}
export function resetAutofocusMessages(): void { recent = []; }
