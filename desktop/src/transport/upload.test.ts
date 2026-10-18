import {describe, expect, it, vi} from "vitest";

// Transport.upload (#512 S9): a raw request body with string headers.
const core = vi.hoisted(() => ({invoke: vi.fn(async () => 2)}));
vi.mock("@tauri-apps/api/core", () => core);

import {tauriTransport} from "./tauriTransport";
import {createWsTransport} from "./wsTransport";
import {isTransportUnsupported, TransportUnsupportedError, upload} from "./index";

describe("Transport.upload", () => {
  it("is Tauri's raw IPC body in the desktop shell: invoke(cmd, bytes, {headers})", async () => {
    core.invoke.mockClear();
    const bytes = new Uint8Array([0x89, 0x50, 0x4e, 0x47]);
    await expect(tauriTransport.upload<number>("review_stage_chart", bytes, {"x-chart-name": "scatter_plot.tiff"})).resolves.toBe(2);
    expect(core.invoke).toHaveBeenCalledTimes(1);
    expect(core.invoke).toHaveBeenCalledWith("review_stage_chart", bytes, {headers: {"x-chart-name": "scatter_plot.tiff"}});
    expect((core.invoke.mock.calls[0] as unknown[])[1]).toBe(bytes); // the same bytes, not a copy or a number array
  });

  it("forwards through the selected transport (Tauri under test)", async () => {
    core.invoke.mockClear();
    const bytes = new Uint8Array([1, 2, 3]);
    await upload("review_stage_chart", bytes, {"x-chart-name": "h.tiff"});
    expect(core.invoke).toHaveBeenCalledWith("review_stage_chart", bytes, {headers: {"x-chart-name": "h.tiff"}});
  });

  it("is refused, typed, over the WebSocket (the server reads no binary requests) and sends nothing", async () => {
    const createSocket = vi.fn();
    const ws = createWsTransport("ws://board/ws", {createSocket});
    const error = await ws.upload("review_stage_chart", new Uint8Array([1]), {"x-chart-name": "s.tiff"}).catch((e: unknown) => e);
    expect(error).toBeInstanceOf(TransportUnsupportedError);
    expect(isTransportUnsupported(error)).toBe(true);
    expect(error).toMatchObject({code: "TRANSPORT_UNSUPPORTED", transportKind: "ws", operation: "review_stage_chart"});
    expect(createSocket).not.toHaveBeenCalled();
    ws.close();
  });

  it("tells other errors apart", () => {
    expect(isTransportUnsupported("TRANSPORT_LOST")).toBe(false);
    expect(isTransportUnsupported(new Error("TRANSPORT_UNSUPPORTED"))).toBe(false);
  });
});
