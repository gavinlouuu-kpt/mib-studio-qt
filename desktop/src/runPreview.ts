// PZ7035 Run-mode preview (#501 P1, `fetch_run_preview`, ABI 27): one PL cell capture with the
// gray frame as fed to the U-Net, its mask and the listed cells of that same frame. The packet
// layout is in bridge-contract.json (`fetch_run_preview`) and PzInstrumentControl.h.
//
// Pure module: unit-testable in plain Node.

export interface RunPreviewCell {
  x: number;
  y: number;
  width: number;
  height: number;
  valid: boolean;
  /** unet_cells_v2 payload words 0-14. */
  payload: number[];
}

export interface RunPreview {
  frameId: number;
  width: number;
  height: number;
  flags: number;
  cells: number;
  blemishes: number;
  dropped: number;
  gray: Uint8Array;
  /** 1 bit per pixel, LSB first, row-major. */
  mask: Uint8Array;
  list: RunPreviewCell[];
}

const HEADER = 32;
const CELL_WORDS = 18;

export function decodeRunPreview(buf: ArrayBuffer): RunPreview {
  if (buf.byteLength < HEADER) throw new Error("run preview: short packet");
  const v = new DataView(buf);
  if (String.fromCharCode(v.getUint8(0), v.getUint8(1), v.getUint8(2), v.getUint8(3)) !== "MIBC")
    throw new Error("run preview: bad magic");
  if (v.getUint16(4, true) !== 1 || v.getUint16(6, true) !== HEADER) throw new Error("run preview: unsupported version");
  const width = v.getUint16(12, true), height = v.getUint16(14, true), listed = v.getUint16(16, true);
  const grayBytes = width * height, maskBytes = grayBytes / 8;
  const expected = HEADER + grayBytes + maskBytes + listed * CELL_WORDS * 4;
  if (buf.byteLength !== expected) throw new Error(`run preview: ${buf.byteLength} bytes, expected ${expected}`);
  const list: RunPreviewCell[] = [];
  for (let c = 0; c < listed; c++) {
    const at = HEADER + grayBytes + maskBytes + c * CELL_WORDS * 4;
    const w = (k: number) => v.getUint32(at + 4 * k, true);
    list.push({
      x: w(15) & 0xffff, y: w(15) >>> 16, width: w(16) & 0xffff, height: w(16) >>> 16,
      valid: (w(17) & 0xffff) !== 0,
      payload: Array.from({length: 15}, (_, k) => w(k)),
    });
  }
  return {
    frameId: v.getUint32(8, true), width, height,
    flags: v.getUint16(18, true), cells: v.getUint16(20, true), blemishes: v.getUint16(22, true),
    dropped: v.getUint32(24, true),
    gray: new Uint8Array(buf, HEADER, grayBytes),
    mask: new Uint8Array(buf, HEADER + grayBytes, maskBytes),
    list,
  };
}

/** RGBA of the gray frame with the U-Net mask tinted, ready for a canvas. */
export function runPreviewRgba(p: RunPreview, showMask = true): Uint8ClampedArray<ArrayBuffer> {
  const out = new Uint8ClampedArray(new ArrayBuffer(p.width * p.height * 4));
  for (let i = 0; i < p.width * p.height; i++) {
    const g = p.gray[i];
    const masked = showMask && (p.mask[i >> 3] >> (i & 7)) & 1;
    out[4 * i] = masked ? Math.min(255, g * 0.5 + 110) : g;
    out[4 * i + 1] = masked ? g * 0.6 : g;
    out[4 * i + 2] = masked ? Math.min(255, g * 0.5 + 110) : g;
    out[4 * i + 3] = 255;
  }
  return out;
}
