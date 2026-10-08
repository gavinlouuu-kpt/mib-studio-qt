// One way to reach the backend (YOFO Studio impl spec S5). Call sites use `invoke` from
// "./transport" with the same command names and camelCase arguments in both shells.
export interface Transport {
  readonly kind: "tauri" | "ws";
  // Resolves with the command's JSON value, or an ArrayBuffer for binary replies (frame
  // packets), exactly like Tauri's invoke; rejects with the command's error string.
  invoke<T>(cmd: string, args?: Record<string, unknown>): Promise<T>;
  readonly session?: SessionSource;
}

// Who controls the instrument (browser transport only; the desktop shell is always its own
// controller). The server tells each client its id and the controller's id: one client at a time
// operates the camera, capture, recording, experiment and hardware, the others only watch.
export interface SessionState {
  clientId?: number;
  controllerId?: number | null;
}
export interface SessionSource {
  get(): SessionState;
  subscribe(listener: (state: SessionState) => void): () => void;
}
