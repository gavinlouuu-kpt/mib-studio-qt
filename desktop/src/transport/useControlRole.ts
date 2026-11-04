import { useSyncExternalStore } from "react";
import { transport } from "./index";
import type { SessionState } from "./Transport";

const none: SessionState = {};
const noSubscription = () => () => undefined;

/** True when another client controls the instrument, so this one can only watch (browser only). */
export function isViewerOnly(state: SessionState): boolean {
  return state.clientId !== undefined && state.controllerId != null && state.controllerId !== state.clientId;
}

/** True when this client controls the instrument: always without a session (the desktop shell is
 *  its own controller); in a browser only while connected and named the controller. A dropped
 *  connection (the session is reset) or another client's claim makes it false. */
export function holdsControl(hasSession: boolean, state: SessionState): boolean {
  return !hasSession || (state.clientId !== undefined && state.controllerId === state.clientId);
}

/** Whether this client controls the instrument (see holdsControl). */
export function useHoldsControl(): boolean {
  const session = transport.session;
  const state = useSyncExternalStore(session ? session.subscribe : noSubscription, session ? session.get : () => none);
  return holdsControl(!!session, state);
}

export function useControlRole(): { viewerOnly: boolean; takeControl: () => Promise<void> } {
  const session = transport.session;
  const state = useSyncExternalStore(session ? session.subscribe : noSubscription, session ? session.get : () => none);
  return { viewerOnly: isViewerOnly(state), takeControl: async () => { await transport.invoke("take_control"); } };
}
