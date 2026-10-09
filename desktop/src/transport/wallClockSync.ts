import { useEffect } from "react";
import { transport } from "./index";
import type { SessionState } from "./Transport";

/** G14: the PZ7035 has no RTC. The controlling browser sends its clock so saved files carry the right date. */
export function isController(state: SessionState): boolean {
  return state.clientId !== undefined && state.controllerId === state.clientId;
}

export const WALL_CLOCK_RESYNC_MS = 10 * 60 * 1000;

/** Sends `sync_wall_clock` when this client becomes the controller and on every `tick()` while it is. */
export function createWallClockSyncer(
  invoke: (cmd: string, args: { unixMs: number }) => Promise<unknown>,
  now: () => number,
) {
  let controller = false;
  const send = () => {
    invoke("sync_wall_clock", { unixMs: Math.round(now()) }).catch(() => undefined); // best effort: the file says if it never synced
  };
  return {
    onSession(state: SessionState) {
      const now_ = isController(state);
      if (now_ && !controller) send();
      controller = now_;
    },
    tick() {
      if (controller) send();
    },
  };
}

/** Browser only (the desktop shell has no session and its host has a clock). */
export function useWallClockSync(): void {
  useEffect(() => {
    const session = transport.session;
    if (!session) return;
    const syncer = createWallClockSyncer((cmd, args) => transport.invoke(cmd, args), () => Date.now());
    syncer.onSession(session.get());
    const unsubscribe = session.subscribe(() => syncer.onSession(session.get()));
    const timer = window.setInterval(() => syncer.tick(), WALL_CLOCK_RESYNC_MS);
    return () => {
      unsubscribe();
      window.clearInterval(timer);
    };
  }, []);
}
