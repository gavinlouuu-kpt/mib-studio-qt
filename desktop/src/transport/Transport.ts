// One way to reach the backend (YOFO Studio impl spec S5). Call sites use `invoke` from
// "./transport" with the same command names and camelCase arguments in both shells.
export interface Transport {
  readonly kind: "tauri" | "ws";
  // Resolves with the command's JSON value, or an ArrayBuffer for binary replies (frame
  // packets), exactly like Tauri's invoke; rejects with the command's error string.
  invoke<T>(cmd: string, args?: Record<string, unknown>): Promise<T>;
}
