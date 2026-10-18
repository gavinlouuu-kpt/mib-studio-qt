// One way to reach the backend (YOFO Studio impl spec S5). Call sites use `invoke` from
// "./transport" with the same command names and camelCase arguments in both shells.
export interface Transport {
  readonly kind: "tauri" | "ws";
  // Resolves with the command's JSON value, or an ArrayBuffer for binary replies (frame
  // packets), exactly like Tauri's invoke; rejects with the command's error string.
  invoke<T>(cmd: string, args?: Record<string, unknown>): Promise<T>;
  // Sends `body` as the command's raw request body with string headers, exactly like Tauri's
  // invoke(cmd, bytes, {headers}) (no JSON number arrays; review chart snapshots, #512 S9).
  // Rejects with TransportUnsupportedError when the transport has no binary request path.
  upload<T>(cmd: string, body: Uint8Array, headers: Record<string, string>): Promise<T>;
  readonly session?: SessionSource;
}

// The transport cannot carry this request at all; the backend was not reached. Callers show
// their own plain words for `operation` (the command name).
export class TransportUnsupportedError extends Error {
  readonly code = "TRANSPORT_UNSUPPORTED";
  readonly transportKind: Transport["kind"];
  readonly operation: string;
  constructor(transportKind: Transport["kind"], operation: string) {
    super(`TRANSPORT_UNSUPPORTED: ${operation} has no ${transportKind} transport path`);
    this.name = "TransportUnsupportedError";
    this.transportKind = transportKind;
    this.operation = operation;
  }
}

export function isTransportUnsupported(error: unknown): error is TransportUnsupportedError {
  return error instanceof TransportUnsupportedError;
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
