import {invoke} from "@tauri-apps/api/core";
import type {Transport} from "./Transport";

// The desktop shell: Tauri IPC into the in-process backend.
export const tauriTransport: Transport = {
  kind: "tauri",
  // Forward exactly what the caller passed (no explicit undefined argument).
  invoke: <T>(cmd: string, args?: Record<string, unknown>) => (args === undefined ? invoke<T>(cmd) : invoke<T>(cmd, args)),
};
