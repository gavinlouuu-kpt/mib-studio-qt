import {invoke} from "@tauri-apps/api/core";
import type {Transport} from "./Transport";

// The desktop shell: Tauri IPC into the in-process backend.
export const tauriTransport: Transport = {
  kind: "tauri",
  // Forward exactly what the caller passed (no explicit undefined argument).
  invoke: <T>(cmd: string, args?: Record<string, unknown>) => (args === undefined ? invoke<T>(cmd) : invoke<T>(cmd, args)),
  // Tauri's raw IPC body: the command reads the bytes and headers from its tauri::ipc::Request.
  upload: <T>(cmd: string, body: Uint8Array, headers: Record<string, string>) => invoke<T>(cmd, body, {headers}),
};
