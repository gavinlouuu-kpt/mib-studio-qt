/// <reference types="vite/client" />
import type {Transport} from "./Transport";
import {tauriTransport} from "./tauriTransport";
import {createWsTransport, wsUrlFromLocation} from "./wsTransport";

export type {Transport} from "./Transport";

// Desktop shell: Tauri IPC. Browser (YOFO Studio on the instrument): the yofo-studio-server
// WebSocket at /ws of the page's origin (or ?server=ws://host:port/ws), token from ?token=.
// VITE_MIB_TRANSPORT=tauri|ws forces one. Unit tests run against the mocked Tauri API.
function select(): Transport {
  const forced = import.meta.env.VITE_MIB_TRANSPORT as string | undefined;
  if (forced === "tauri") return tauriTransport;
  if (forced === "ws") return createWsTransport(wsUrlFromLocation());
  const inTauri = typeof window !== "undefined" && "__TAURI_INTERNALS__" in window;
  if (inTauri || import.meta.env.MODE === "test") return tauriTransport;
  return createWsTransport(wsUrlFromLocation());
}

export const transport: Transport = select();

// Drop-in for Tauri's invoke at every call site.
export function invoke<T>(cmd: string, args?: Record<string, unknown>): Promise<T> {
  return args === undefined ? transport.invoke<T>(cmd) : transport.invoke<T>(cmd, args);
}

// True in a browser against the instrument: native dialogs, file reveal and the desktop
// updater are unavailable there.
export const isRemote = transport.kind === "ws";
