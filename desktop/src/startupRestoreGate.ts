// The order of the two startup writes of the autofocus target ring width (#883; Gavin, 2026-11-07, MIBQT-45: "Latest should
// win ... The user's latest adjustments should come through"). When a backend starts, the startup configuration restore
// (profiles.tsx, #398 M2c) re-applies the last profile or method, which carries its own `autofocus_focus_setpoint`, and the
// nanopositioner panel (AutofocusControls) restores the operator's remembered value. The panel waits here until the startup
// restore has settled (re-applied, failed, or not run), so the remembered value is always written last and is never
// overwritten by the restore afterwards. One gate per backend session: `resetStartupRestore` when the backend goes away.

let settled = false;
let captureTaken = false;
let waiters: Array<() => void> = [];

/** Called once the startup restore of this backend session finished or was skipped. Later calls change nothing. */
export function settleStartupRestore(): void {
  if (settled) return;
  settled = true;
  const pending = waiters;
  waiters = [];
  for (const resolve of pending) resolve();
}

/** The backend went away: the next session runs its own startup restore. Waiters keep waiting for that one. */
export function resetStartupRestore(): void { settled = false; captureTaken = false; }

/** Resolves when the startup restore of the current backend session has settled. */
export function startupRestoreSettled(): Promise<void> {
  return settled ? Promise.resolve() : new Promise(resolve => { waiters.push(resolve); });
}

/** True for the first caller after the restore settled in this backend session, false afterwards (and before it settled).
 *  The nanopositioner panel takes the configured target ring width as its Home value then and only then, so a panel that is
 *  mounted again later in the session keeps that Home instead of taking the operator's live value. */
export function takeStartupSetpointCapture(): boolean {
  if (!settled || captureTaken) return false;
  captureTaken = true;
  return true;
}
