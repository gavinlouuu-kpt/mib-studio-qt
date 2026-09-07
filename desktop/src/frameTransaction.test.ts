import { beforeEach, expect, it, vi } from "vitest";
import { bridge } from "./bridge";
const { invoke } = vi.hoisted(() => ({ invoke: vi.fn() }));
vi.mock("@tauri-apps/api/core", () => ({ invoke }));
function packet(index: bigint, value: number, kind: number, width = 2, height = 2) {
  const buf = new ArrayBuffer(96 + width * height);
  const d = new DataView(buf);
  new Uint8Array(buf).set([77, 73, 66, 70]);
  d.setUint16(4, 1, true); d.setUint16(6, 96, true);
  d.setUint32(8, 1, true); d.setUint32(12, kind, true);
  [index, 18446744073709551615n, BigInt(width), BigInt(height), 0x01080001n, BigInt(width), BigInt(width * height), 0n, 0n, 0n]
    .forEach((v, i) => d.setBigUint64(16 + i * 8, v, true));
  new Uint8Array(buf, 96).fill(value);
  return buf;
}
beforeEach(() => { invoke.mockReset(); });
it("keeps live metadata and pixels paired when an indexed pull intervenes", async () => {
  let last = new Uint8Array();
  invoke.mockImplementation(async (command: string) => {
    if (command === "fetch_frame") {
      last = new Uint8Array([11, 11, 11, 11]);
      return { valid: true, frame_index: 17, width: 2, height: 2 };
    }
    if (command === "fetch_frame_by_index") {
      last = new Uint8Array([22, 22, 22, 22]);
      return { valid: true, frame_index: 18, width: 2, height: 2 };
    }
    if (command === "frame_bytes") return last.buffer;
    if (command === "fetch_frame_packet") return packet(17n, 11, 1);
    if (command === "fetch_indexed_frame_packet") return packet(18n, 22, 2);
    throw new Error(command);
  });
  const a = await bridge.fetchFrame();
  const b = await bridge.fetchFrameByIndex(18);
  const pixels = "data" in a ? a.data : await bridge.frameBytes();
  expect(Array.from(pixels as Uint8Array)).toEqual([11, 11, 11, 11]);
  expect(String(a.frame_index)).toBe("17");
  expect(String(b.frame_index)).toBe("18");
});

it("out-of-order native responses preserve exact u64 identity, geometry and checksum", async () => {
  let finishA!: (b: ArrayBuffer) => void;
  let finishB!: (b: ArrayBuffer) => void;
  invoke.mockImplementation((command: string) => new Promise<ArrayBuffer>(resolve => {
    if (command === "fetch_frame_packet") finishA = resolve;
    else if (command === "fetch_indexed_frame_packet") finishB = resolve;
  }));
  const a = bridge.fetchFrame();
  const b = bridge.fetchFrameByIndex("18446744073709551615");
  finishB(packet(18446744073709551615n, 22, 2, 3, 1));
  const second = await b;
  finishA(packet(9007199254740993n, 11, 1));
  const first = await a;
  expect(first.frame_index).toBe("9007199254740993");
  expect(second.frame_index).toBe("18446744073709551615");
  expect(first.data.reduce((a,b) => a+b,0)).toBe(44);
  expect(second.data.reduce((a,b) => a+b,0)).toBe(66);
  expect([second.width, second.height, second.stride_bytes]).toEqual([3,1,3]);
  expect([first.width, first.height, first.stride_bytes]).toEqual([2,2,2]);
  expect(invoke.mock.calls[1][1]).toEqual({frameIndex:"18446744073709551615"});
});
it("rejects a response from before a source mutation", async () => {
  let finish!: (b: ArrayBuffer) => void;
  invoke.mockImplementation((command: string) => command === "fetch_frame_packet"
    ? new Promise<ArrayBuffer>(r => { finish=r; }) : Promise.resolve({ok:true}));
  const a = bridge.fetchFrame();
  const rejected = expect(a).rejects.toThrow("FRAME_REPLY_STALE");
  await bridge.configureMock("explicit-fixture", 1, true);
  finish(packet(17n,11,1)); await rejected;
});

it("10,000 interleaved client pulls retain only their own pixels", async () => {
  let index=9007199254740993n;
  invoke.mockImplementation(async (command:string) => {
    const kind=command === "fetch_frame_packet" ? 1 : 2;
    return packet(index++,kind*11,kind,kind+1,1);
  });
  for(let i=0;i<5000;i++) {
    const a=bridge.fetchFrame(), b=bridge.fetchFrameByIndex(String(i));
    const second=await b, first=await a;
    expect(BigInt(second.frame_index)-BigInt(first.frame_index)).toBe(1n);
    expect(Array.from(first.data)).toEqual([11,11]);
    expect(Array.from(second.data)).toEqual([22,22,22]);
    expect(first.data.buffer.byteLength+second.data.buffer.byteLength).toBe(197);
  }
});
