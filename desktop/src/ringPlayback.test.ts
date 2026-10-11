import { describe, expect, it } from "vitest";
import {
  advancePlayback, capacityText, cellMetrics, faultClearedNote, frameNotes, metricText, clampSeq, contourPixels, decodeRingFrame, elapsedText, nextOverlay, noMaskNote,
  playbackAvailability, rangeText, ringFrameRgba, saveClipState, secondsText, startDiscardNotice, stepSeq, type RingFrame, type RingStatus,
} from "./ringPlayback";

function packet(opts: { seq?: number; w?: number; h?: number; cells?: number; mask?: boolean; truncated?: boolean; flags?: number } = {}): ArrayBuffer {
  const w = opts.w ?? 16, h = opts.h ?? 4, cells = opts.cells ?? 2;
  const gray = w * h, maskBytes = gray / 8;
  const buf = new ArrayBuffer(48 + gray + maskBytes + cells * 19 * 4);
  const v = new DataView(buf);
  "MIBR".split("").forEach((c, i) => v.setUint8(i, c.charCodeAt(0)));
  v.setUint16(4, 1, true); v.setUint16(6, 48, true);
  v.setBigUint64(8, BigInt(opts.seq ?? 7), true); v.setBigUint64(16, 1007n, true); v.setBigUint64(24, 5_035_000n, true);
  v.setUint32(32, 100_000_000, true); v.setUint32(36, 0, true);
  v.setUint16(40, w, true); v.setUint16(42, h, true); v.setUint16(44, cells, true);
  v.setUint16(46, (opts.mask === false ? 0 : 1) | (opts.truncated ? 2 : 0) | (opts.flags ?? 0), true);
  for (let i = 0; i < gray; i++) v.setUint8(48 + i, i % 251);
  for (let i = 0; i < maskBytes; i++) v.setUint8(48 + gray + i, i === 1 ? 0xff : 0);
  for (let c = 0; c < cells; c++) {
    const at = 48 + gray + maskBytes + c * 76;
    for (let k = 0; k < 15; k++) v.setUint32(at + 4 * k, 1000 * (c + 1) + k, true);
    v.setUint32(at + 60, 3 | (1 << 16), true); v.setUint32(at + 64, 5 | (2 << 16), true); v.setUint32(at + 68, (cells << 24) | (c << 16) | (c === 0 ? 1 : 0), true); v.setUint32(at + 72, 0x7fff, true);
  }
  return buf;
}

const status = (over: Partial<RingStatus> = {}): RingStatus => ({
  available: true, frozen: true, invalid: false, stop_incomplete: false, restore_needed: false, run_frozen: true, capacity_frames: 5000, first_seq: 120, last_seq: 5119,
  count: 5000, sensor_fps: 5000.8, ...over,
});

describe("MIBR packet", () => {
  it("decodes the header, the blocks and the cells", () => {
    const f = decodeRingFrame(packet());
    expect(f.seq).toBe(7);
    expect(f.frameId).toBe(1007);
    expect(f.tickHz).toBe(100_000_000);
    expect(f.width).toBe(16);
    expect(f.height).toBe(4);
    expect(f.maskPresent).toBe(true);
    expect(f.gray.length).toBe(64);
    expect(f.mask.length).toBe(8);
    expect(f.cells).toHaveLength(2);
    expect(f.cells[0]).toMatchObject({ x: 3, y: 1, width: 5, height: 2, valid: true });
    expect(f.cells[1].valid).toBe(false);
    expect(f.cells[1].index).toBe(1);
    expect(f.cells[0].payloadValidity).toBe(0x7fff);
    expect(f.cells[0].payload[3]).toBe(1003);
    expect(f.cells[1].payload[14]).toBe(2014);
  });
  it("flags a missing mask and truncated results", () => {
    const f = decodeRingFrame(packet({ mask: false, truncated: true }));
    expect(f.maskPresent).toBe(false);
    expect(f.resultsTruncated).toBe(true);
    expect(noMaskNote(f)).toMatch(/No result for this frame/);
  });
  it("carries a cut frame, an invalid frame and an incomplete mask, and says so", () => {
    const f = decodeRingFrame(packet({ mask: false, flags: 4 | 8 | 16 }));
    expect(f.frameInvalid).toBe(true);
    expect(f.cut).toBe(true);
    expect(f.maskIncomplete).toBe(true);
    expect(f.maskPresent).toBe(false);
    expect(frameNotes(f)).toHaveLength(2);
    expect(frameNotes(f)[0]).toMatch(/cut/);
    expect(noMaskNote(f)).toMatch(/incomplete/);
    expect(frameNotes(decodeRingFrame(packet()))).toEqual([]);
  });
  it("rejects a malformed packet", () => {
    expect(() => decodeRingFrame(new ArrayBuffer(8))).toThrow(/short/);
    const bad = packet();
    new DataView(bad).setUint8(0, 0x58);
    expect(() => decodeRingFrame(bad)).toThrow(/magic/);
    expect(() => decodeRingFrame(packet().slice(0, 100))).toThrow(/bytes, expected/);
  });
});

describe("capacity, range and availability", () => {
  it("shows frames and seconds at the sensor rate", () => {
    expect(capacityText({ capacity_frames: 5000, sensor_fps: 5000.8 })).toBe("5,000 frames · 1.00 s at 5,001 fps");
    expect(capacityText({ capacity_frames: 5000, sensor_fps: 0 })).toBe("5,000 frames");
    expect(secondsText(26)).toBe("26.0 s");
    expect(secondsText(130)).toBe("130 s");
    expect(secondsText(Number.NaN)).toBe("—");
    expect(rangeText(status())).toBe("Frames 120 to 5,119 · 5,000 frames · 1.00 s");
    expect(rangeText(status({ count: 0 }))).toBe("No buffered frames");
  });
  it("offers playback only for a frozen, valid, stopped ring with frames", () => {
    expect(playbackAvailability(status()).ok).toBe(true);
    expect(playbackAvailability(null).ok).toBe(false);
    expect(playbackAvailability(status({ available: false, reason: "frame ring: boot with a smaller mem=" })).reason).toMatch(/mem=/);
    expect(playbackAvailability(status({ invalid: true, reason: "the ring stalled (RING_STALLED)" })).reason).toMatch(/RING_STALLED/);
    expect(playbackAvailability(status({ run_frozen: false })).reason).toMatch(/Stop Run/);
    expect(playbackAvailability(status({ frozen: false })).ok).toBe(false);
    // STOP_STUCK: not frozen, but the frames below the newest stay readable, flagged.
    const stuck = playbackAvailability(status({ frozen: false, stop_incomplete: true }));
    expect(stuck.ok).toBe(true);
    expect(stuck.note).toMatch(/Stop incomplete/);
    expect(playbackAvailability(status()).note).toBeUndefined();
    // A stop that never reached IDLE: a hardware fault, restore needed, never playback.
    expect(playbackAvailability(status({ frozen: false, restore_needed: true })).reason).toMatch(/Restore needed/);
    expect(playbackAvailability(status({ invalid: true, stop_incomplete: true, reason: "RING_STALLED" })).ok).toBe(false);
    expect(playbackAvailability(status({ count: 0 })).reason).toMatch(/empty/);
  });
  it("shows the elapsed time from the first buffered frame", () => {
    const first = { timestampTicks: 1_000_000 }, f = { timestampTicks: 1_020_000, tickHz: 100_000_000 };
    expect(elapsedText(f, first)).toBe("+0.0002 s");
    expect(elapsedText(f, null)).toBe("");
  });
  it("keeps Save clip disabled, with the reason, until the SSD path exists", () => {
    expect(saveClipState(false)).toEqual({ enabled: false, reason: "Saving needs the SSD (not available yet)." });
    expect(saveClipState(true).enabled).toBe(true);
  });
});

describe("scrub, step and play", () => {
  const r = { first: 120, last: 5119 };
  it("clamps and steps inside the buffered range", () => {
    expect(clampSeq(0, r)).toBe(120);
    expect(clampSeq(99999, r)).toBe(5119);
    expect(clampSeq(200.4, r)).toBe(200);
    expect(clampSeq(Number.NaN, r)).toBe(120);
    expect(stepSeq(5119, 1, r)).toBe(5119);
    expect(stepSeq(120, -1, r)).toBe(120);
    expect(stepSeq(500, 8, r)).toBe(508);
  });
  it("advances at the display rate and carries the remainder", () => {
    let st = { seq: 120, carry: 0, ended: false };
    st = advancePlayback(st.seq, st.carry, 16, 30, r); // 0.48 frames
    expect(st).toEqual({ seq: 120, carry: 0.48, ended: false });
    st = advancePlayback(st.seq, st.carry, 17, 30, r); // 0.48 + 0.51 = 0.99
    expect(st.seq).toBe(120);
    st = advancePlayback(st.seq, st.carry, 1, 30, r); // crosses 1
    expect(st.seq).toBe(121);
    expect(advancePlayback(1000, 0, 1000, 60, r).seq).toBe(1060);
    expect(advancePlayback(1000, 0, -5, 60, r).seq).toBe(1000);
  });
  it("stops at the end, or wraps when asked to loop", () => {
    expect(advancePlayback(5110, 0, 1000, 30, r)).toEqual({ seq: 5119, carry: 0, ended: true });
    expect(advancePlayback(5110, 0, 1000, 30, r, true)).toMatchObject({ seq: 120 + ((5110 + 30 - 120) % 5000), ended: false });
  });
});

describe("overlays", () => {
  const frame = (): RingFrame => decodeRingFrame(packet());
  it("cycles Off, Mask, Contours, Both", () => {
    expect(nextOverlay("off")).toBe("mask");
    expect(nextOverlay("mask")).toBe("contours");
    expect(nextOverlay("contours")).toBe("both");
    expect(nextOverlay("both")).toBe("off");
  });
  it("outlines a region: interior pixels are not contour", () => {
    // 8 x 5 mask with a 4 x 3 block at (2,1)
    const w = 8, h = 5, mask = new Uint8Array(w * h / 8);
    for (let y = 1; y <= 3; y++) for (let x = 2; x <= 5; x++) mask[(y * w + x) >> 3] |= 1 << ((y * w + x) & 7);
    const c = contourPixels(mask, w, h);
    expect(c[1 * w + 2]).toBe(1);
    expect(c[2 * w + 3]).toBe(0); // inside
    expect(c[2 * w + 4]).toBe(0);
    expect(c[3 * w + 5]).toBe(1);
    expect(c[0]).toBe(0); // not in the mask
    expect(c.reduce((a, b) => a + b, 0)).toBe(10); // 12 pixels minus the 2 interior
  });
  it("draws gray, tint and outline", () => {
    const f = frame(); // mask byte 1 set: pixels 8..15 (row 0, x 8..15)
    const off = ringFrameRgba(f, "off"), mask = ringFrameRgba(f, "mask"), both = ringFrameRgba(f, "both");
    expect(off[4 * 8]).toBe(f.gray[8]);
    expect(off[4 * 8 + 1]).toBe(f.gray[8]);
    expect(mask[4 * 8 + 1]).not.toBe(f.gray[8]); // tinted
    expect(mask[4 * 0 + 1]).toBe(f.gray[0]); // outside the mask
    expect([both[4 * 8], both[4 * 8 + 1], both[4 * 8 + 2]]).toEqual([255, 176, 0]); // row 0 is the border: outline
    const contours = ringFrameRgba(f, "contours");
    expect(contours[4 * 9 + 1]).toBe(176);
    expect(contours[4 * 0]).toBe(f.gray[0]);
    expect(off[3]).toBe(255);
  });
});

describe("cell measurements", () => {
  it("decodes the unet_cells_v2 words in host units, NaN where the PL left a word out", () => {
    const payload = new Array(15).fill(0);
    payload[0] = 3 | (0 << 16) | (1 << 24); // object 3, valid, target
    payload[1] = Math.round(120.5 * 65536); payload[2] = Math.round(130.25 * 65536); payload[4] = Math.round(0.92 * 65536);
    payload[5] = Math.round(0.05 * 65536) | (4 << 16) | (2 << 24); // deformability 0.05, 2 cells
    payload[6] = Math.round(88.5 * 65536); payload[7] = (-12.5 * 65536) | 0; payload[9] = Math.round(300 * 65536);
    payload[10] = Math.round(1.75 * 65536); payload[11] = Math.round(4.5 * 256); payload[13] = 410 | (3 << 16);
    const full = cellMetrics({ payload, payloadValidity: 0x7fff });
    expect(full).toMatchObject({ objectId: 3, reason: 0, cutOff: false, target: true, pixelCount: 410, blemishCount: 3 });
    expect(full.contourArea).toBeCloseTo(120.5, 4);
    expect(full.hullArea).toBeCloseTo(130.25, 4);
    expect(full.deformability).toBeCloseTo(0.05, 4);
    expect(full.youngsModulusKpa).toBeCloseTo(1.75, 4);
    expect(full.centroidX).toBeCloseTo(-12.5, 4);
    expect(full.laplacianVariance).toBeCloseTo(4.5, 4);
    const partial = cellMetrics({ payload, payloadValidity: 0b11 }); // only words 0 and 1
    expect(partial.contourArea).toBeCloseTo(120.5, 4);
    expect(Number.isNaN(partial.hullArea)).toBe(true); // word 2 left out: not available, never 0
    const rejected = cellMetrics({ payload, payloadValidity: 0b1 }); // a rejected cell: only word 0
    expect(Number.isNaN(rejected.contourArea) && Number.isNaN(rejected.hullArea)).toBe(true);
    expect(metricText(rejected.hullArea, 1)).toBe("—");
    expect(Number.isNaN(partial.deformability)).toBe(true);
    expect(Number.isNaN(partial.youngsModulusKpa)).toBe(true);
    expect(metricText(partial.deformability)).toBe("—");
    expect(metricText(full.deformability, 3)).toBe("0.050");
  });
});

describe("a fault cleared at Run start", () => {
  it("is named, with its register value, and empty when nothing was cleared", () => {
    expect(faultClearedNote({ fault_cleared: 0x104 })).toBe("PL fault 0x104 cleared at Run start");
    expect(faultClearedNote({ fault_cleared: 0 })).toBe("");
    expect(faultClearedNote({})).toBe("");
    expect(faultClearedNote({ fault_cleared: 0, fault_cleared_state: 6 })).toBe("PL in FAULT state (FAULT register 0x0) cleared at Run start");
    expect(faultClearedNote(null)).toBe("");
  });
});

describe("starting an experiment from a stopped Run", () => {
  it("names the buffered frames it discards, and says nothing when no Run is stopped", () => {
    expect(startDiscardNotice(status())).toBe("Starting an experiment discards the 5,000 buffered frames of the stopped Run. Continue?");
    expect(startDiscardNotice(status({ count: 0 }))).toBe("Starting an experiment ends the stopped Run. Continue?");
    expect(startDiscardNotice(status({ run_frozen: false }))).toBe("");
    expect(startDiscardNotice(null)).toBe("");
    expect(startDiscardNotice(undefined)).toBe("");
  });
  it("the ring status carries the epoch of the ARM (ABI 37), absent on an older server", () => {
    expect(status({ epoch: 7 }).epoch).toBe(7);
    expect(status().epoch).toBeUndefined();
  });
});
