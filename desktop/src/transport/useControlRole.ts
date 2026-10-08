import { useSyncExternalStore } from "react";
import { transport } from "./index";
import type { SessionState } from "./Transport";

const none: SessionState = {};
const noSubscription = () => () => undefined;

/** True when another client controls the instrument, so this one can only watch (browser only). */
export function isViewerOnly(state: SessionState): boolean {
  return state.clientId !== undefined && state.controllerId != null && state.controllerId !== state.clientId;
}

export function useControlRole(): { viewerOnly: boolean; takeControl: () => Promise<void> } {
  const session = transport.session;
  const state = useSyncExternalStore(session ? session.subscribe : noSubscription, session ? session.get : () => none);
  return { viewerOnly: isViewerOnly(state), takeControl: async () => { await transport.invoke("take_control"); } };
}
