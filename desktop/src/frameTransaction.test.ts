import { beforeEach, expect, it, vi } from "vitest";
import { bridge } from "./bridge";
const { invoke } = vi.hoisted(() => ({ invoke: vi.fn() }));
vi.mock("@tauri-apps/api/core", () => ({ invoke }));
function packet(index: bigint, value: number, kind: number) {
  const buf = new ArrayBuffer(100);
  const d = new DataView(buf);
  new Uint8Array(buf).set([77, 73, 66, 70]);
  d.setUint16(4, 1, true); d.setUint16(6, 96, true);
  d.setUint32(8, 1, true); d.setUint32(12, kind, true);
  [index, 18446744073709551615n, 2n, 2n, 0x01080001n, 2n, 4n, 0n, 0n, 0n]
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
