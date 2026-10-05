import {describe, expect, it} from "vitest";
import {createWsTransport, wsUrlFromLocation} from "./wsTransport";

class FakeSocket {
  static OPEN = 1;
  readyState = 0;
  binaryType = "blob";
  sent: string[] = [];
  onopen?: () => void;
  onclose?: () => void;
  onerror?: () => void;
  onmessage?: (message: {data: unknown}) => void;
  constructor(public url: string) {}
  send(text: string) { this.sent.push(text); }
  close() { this.readyState = 3; this.onclose?.(); }
  open() { this.readyState = 1; this.onopen?.(); }
  reply(value: unknown) { this.onmessage?.({data: JSON.stringify(value)}); }
  binary(id: number, bytes: number[]) {
    const buffer = new ArrayBuffer(8 + bytes.length);
    new DataView(buffer).setBigUint64(0, BigInt(id), true);
    new Uint8Array(buffer, 8).set(bytes);
    this.onmessage?.({data: buffer});
  }
  lastRequest() { return JSON.parse(this.sent[this.sent.length - 1]); }
}

function setup() {
  const sockets: FakeSocket[] = [];
  (globalThis as {WebSocket?: unknown}).WebSocket ??= {OPEN: 1};
  const transport = createWsTransport("ws://board/ws?token=t", {
    reconnectMs: 0,
    createSocket: (url) => { const s = new FakeSocket(url); sockets.push(s); return s as unknown as WebSocket; },
  });
  return {transport, sockets};
}

const tick = () => new Promise((resolve) => setTimeout(resolve, 0));

describe("wsTransport", () => {
  it("correlates JSON and binary replies by request id", async () => {
    const {transport, sockets} = setup();
    const started = transport.invoke<{ok: boolean}>("start_capture");
    await tick();
    sockets[0].open();
    await tick();
    const request = sockets[0].lastRequest();
    expect(request).toEqual({request_id: request.request_id, cmd: "start_capture", args: {}});
    sockets[0].reply({request_id: request.request_id, ok: {ok: true}});
    await expect(started).resolves.toEqual({ok: true});

    const packet = transport.invoke<ArrayBuffer>("fetch_frame_packet");
    await tick();
    const id = sockets[0].lastRequest().request_id;
    sockets[0].binary(id, [0x4d, 0x49, 0x42, 0x46]);
    expect(new Uint8Array(await packet)).toEqual(new Uint8Array([0x4d, 0x49, 0x42, 0x46]));

    const failed = transport.invoke("no_such_command", {x: 1});
    await tick();
    const failedRequest = sockets[0].lastRequest();
    expect(failedRequest.args).toEqual({x: 1});
    sockets[0].reply({request_id: failedRequest.request_id, error: "UNKNOWN_COMMAND: no_such_command"});
    await expect(failed).rejects.toBe("UNKNOWN_COMMAND: no_such_command");
  });

  it("answers poll_events_exact from pushed envelopes, never asking the server", async () => {
    const {transport, sockets} = setup();
    const first = transport.invoke("is_initialized");
    await tick();
    sockets[0].open();
    await tick();
    sockets[0].reply({request_id: sockets[0].lastRequest().request_id, ok: true});
    await first;
    sockets[0].reply({event: {transport_version: 1, events: [{kind: "CameraStatus"}, {kind: "CameraStatus"}]}});
    sockets[0].reply({event: {transport_version: 1, events: [{kind: "RecordingStatus"}]}});
    const sent = sockets[0].sent.length;
    await expect(transport.invoke("poll_events_exact")).resolves.toEqual({
      transport_version: 1, events: [{kind: "CameraStatus"}, {kind: "CameraStatus"}, {kind: "RecordingStatus"}],
    });
    await expect(transport.invoke("poll_events_exact")).resolves.toEqual({transport_version: 1, events: []});
    expect(sockets[0].sent.length).toBe(sent);
  });

  it("rejects calls in flight when the link drops and reconnects on the next call", async () => {
    const {transport, sockets} = setup();
    const call = transport.invoke("start_capture");
    await tick();
    sockets[0].open();
    await tick();
    sockets[0].close();
    await expect(call).rejects.toBe("TRANSPORT_LOST");
    const again = transport.invoke<boolean>("is_initialized");
    await tick();
    expect(sockets.length).toBe(2);
    sockets[1].open();
    await tick();
    sockets[1].reply({request_id: sockets[1].lastRequest().request_id, ok: true});
    await expect(again).resolves.toBe(true);
  });

  it("derives the socket URL from the page", () => {
    const at = (href: string) => new URL(href) as unknown as Location;
    expect(wsUrlFromLocation(at("http://192.168.137.2:8427/?token=a%20b"))).toBe("ws://192.168.137.2:8427/ws?token=a%20b");
    expect(wsUrlFromLocation(at("https://board/"))).toBe("wss://board/ws");
    expect(wsUrlFromLocation(at("http://localhost:1420/?server=ws://board:8427/ws&token=t"))).toBe("ws://board:8427/ws?token=t");
  });
});
