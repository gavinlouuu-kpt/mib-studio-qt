import type {Transport} from "./Transport";

// A browser talking to yofo-studio-server (crates/mib-bridge-server):
//   -> {"request_id", "cmd", "args"}
//   <- {"request_id", "ok"} | {"request_id", "error"}, or binary: u64 LE request id + bytes
//   <- {"event": EventEnvelope} pushed by the server, which alone drains backend events.
// `poll_events_exact` is answered locally from the pushed envelopes, so the UI's event loop
// is the same in both shells. The socket opens on first use and reopens after a loss; calls
// in flight when it drops reject with TRANSPORT_LOST.

const MAX_EVENTS = 4097; // json_transport.max_events (bridge-contract.json)

type Pending = {resolve: (value: unknown) => void; reject: (error: unknown) => void};
type Envelope = {transport_version: number; events: unknown[]};

export interface WsTransportOptions {
  // Override the socket factory (tests).
  createSocket?: (url: string) => WebSocket;
  // Delay before reopening a lost socket.
  reconnectMs?: number;
}

export function wsUrlFromLocation(location: Location = window.location): string {
  const params = new URLSearchParams(location.search);
  const explicit = params.get("server");
  const base = explicit ?? `${location.protocol === "https:" ? "wss:" : "ws:"}//${location.host}/ws`;
  const token = params.get("token");
  return token ? `${base}${base.includes("?") ? "&" : "?"}token=${encodeURIComponent(token)}` : base;
}

export function createWsTransport(url: string, options: WsTransportOptions = {}): Transport & {close(): void} {
  const createSocket = options.createSocket ?? ((u: string) => new WebSocket(u));
  const reconnectMs = options.reconnectMs ?? 1000;
  const pending = new Map<number, Pending>();
  let nextId = 1;
  let socket: WebSocket | undefined;
  let opening: Promise<WebSocket> | undefined;
  let queued: unknown[] = [];
  let transportVersion = 1;
  let closed = false;
  let lastFailure = 0;

  function fail(error: string) {
    for (const p of pending.values()) p.reject(error);
    pending.clear();
  }

  function open(): Promise<WebSocket> {
    if (socket && socket.readyState === WebSocket.OPEN) return Promise.resolve(socket);
    if (opening) return opening;
    const wait = Math.max(0, lastFailure + reconnectMs - Date.now());
    opening = new Promise<WebSocket>((resolve, reject) => {
      setTimeout(() => {
        const ws = createSocket(url);
        ws.binaryType = "arraybuffer";
        ws.onopen = () => { socket = ws; opening = undefined; resolve(ws); };
        ws.onerror = () => { /* onclose follows */ };
        ws.onclose = () => {
          const wasOpen = socket === ws;
          socket = undefined;
          opening = undefined;
          lastFailure = Date.now();
          fail("TRANSPORT_LOST");
          if (!wasOpen) reject("TRANSPORT_UNAVAILABLE");
        };
        ws.onmessage = (message: MessageEvent) => receive(message.data);
      }, wait);
    });
    return opening;
  }

  function receive(data: unknown) {
    if (data instanceof ArrayBuffer) {
      if (data.byteLength < 8) return;
      const id = Number(new DataView(data).getBigUint64(0, true));
      const p = pending.get(id);
      if (p) { pending.delete(id); p.resolve(data.slice(8)); }
      return;
    }
    let value: {request_id?: number; ok?: unknown; error?: unknown; event?: Envelope};
    try { value = JSON.parse(String(data)); } catch { return; }
    if (value.event) {
      transportVersion = value.event.transport_version;
      queued = queued.concat(value.event.events ?? []);
      return;
    }
    if (typeof value.request_id !== "number") return;
    const p = pending.get(value.request_id);
    if (!p) return;
    pending.delete(value.request_id);
    if ("error" in value) p.reject(value.error);
    else p.resolve(value.ok);
  }

  async function invoke<T>(cmd: string, args?: Record<string, unknown>): Promise<T> {
    if (closed) throw "TRANSPORT_CLOSED";
    if (cmd === "poll_events_exact") {
      const events = queued.slice(0, MAX_EVENTS);
      queued = queued.slice(events.length);
      void open().catch(() => undefined); // keep the event stream alive
      return {transport_version: transportVersion, events} as T;
    }
    const ws = await open();
    const id = nextId++;
    return new Promise<T>((resolve, reject) => {
      pending.set(id, {resolve: resolve as (value: unknown) => void, reject});
      ws.send(JSON.stringify({request_id: id, cmd, args: args ?? {}}));
    });
  }

  return {
    kind: "ws",
    invoke,
    close() { closed = true; socket?.close(); fail("TRANSPORT_CLOSED"); },
  };
}
