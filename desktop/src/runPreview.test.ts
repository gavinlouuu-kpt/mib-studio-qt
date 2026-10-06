import { describe, expect, it } from "vitest";
import { decodeRunPreview, runPreviewRgba } from "./runPreview";

// Mirrors pz::encodeRunPreview (PzInstrumentControl.cpp).
function packet(listed = 1, w = 512, h = 96): ArrayBuffer {
  const gray = w * h, mask = gray / 8;
  const buf = new ArrayBuffer(32 + gray + mask + listed * 72);
  const v = new DataView(buf);
  "MIBC".split("").forEach((c, i) => v.setUint8(i, c.charCodeAt(0)));
  v.setUint16(4, 1, true); v.setUint16(6, 32, true); v.setUint32(8, 777, true);
  v.setUint16(12, w, true); v.setUint16(14, h, true); v.setUint16(16, listed, true);
  v.setUint16(18, 5, true); v.setUint16(20, 4, true); v.setUint16(22, 1, true); v.setUint32(24, 3, true);
  new Uint8Array(buf, 32, gray).fill(80);
  new Uint8Array(buf, 32 + gray, mask)[0] = 0b0000_0010; // pixel 1 masked
  for (let c = 0; c < listed; c++) {
    const at = 32 + gray + mask + c * 72;
    v.setUint32(at + 4 * 15, (11 << 16) | 140, true);
    v.setUint32(at + 4 * 16, (20 << 16) | 30, true);
    v.setUint32(at + 4 * 17, (listed << 24) | (c << 16) | 1, true);
  }
  return buf;
}

describe("run preview packet (#501 P1)", () => {
  it("decodes header, gray, mask and cell boxes", () => {
    const p = decodeRunPreview(packet(2));
    expect(p).toMatchObject({frameId: 777, width: 512, height: 96, flags: 5, cells: 4, blemishes: 1, dropped: 3});
    expect(p.gray.length).toBe(49152);
    expect(p.list).toHaveLength(2);
    expect(p.list[0]).toMatchObject({x: 140, y: 11, width: 30, height: 20, valid: true});
  });

  it("tints masked pixels only", () => {
    const rgba = runPreviewRgba(decodeRunPreview(packet(0)));
    expect([rgba[0], rgba[1], rgba[2]]).toEqual([80, 80, 80]);
    expect(rgba[4 + 1]).toBeLessThan(80);
    expect(Array.from(runPreviewRgba(decodeRunPreview(packet(0)), false).slice(4, 7))).toEqual([80, 80, 80]);
  });

  it("rejects truncated or foreign packets", () => {
    expect(() => decodeRunPreview(packet(1).slice(0, 1000))).toThrow();
    const bad = packet(0);
    new DataView(bad).setUint8(0, 0x58);
    expect(() => decodeRunPreview(bad)).toThrow(/magic/);
  });
});
