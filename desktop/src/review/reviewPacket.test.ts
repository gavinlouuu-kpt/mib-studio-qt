import { describe, expect, it } from "vitest";
import { FRAME_PACKET } from "../bridgeContract";
import { decodeReviewPacket } from "./reviewPacket";
import { REVIEW_FRAME_PACKET } from "./reviewContract";

// Builds a packet the way desktop/src-tauri/src/review_packet.rs encodes one.
function packet(kind: number, width: number, height: number, format: number, bpp: number,
                identity: Partial<Record<72 | 80 | 88, bigint>> = {}): ArrayBuffer {
  const stride = width * bpp, len = stride * height;
  const buf = new ArrayBuffer(FRAME_PACKET.header_bytes + len);
  const d = new DataView(buf);
  d.setUint32(0, 0x4642494d, true);
  d.setUint16(4, FRAME_PACKET.version, true);
  d.setUint16(6, FRAME_PACKET.header_bytes, true);
  d.setUint32(8, 1, true);
  d.setUint32(12, kind, true);
  const fields: [number, bigint][] = [[16, 7n], [32, BigInt(width)], [40, BigInt(height)],
    [48, BigInt(format)], [56, BigInt(stride)], [64, BigInt(len)]];
  for (const [o, v] of fields) d.setBigUint64(o, v, true);
  for (const [o, v] of Object.entries(identity)) d.setBigUint64(Number(o), v as bigint, true);
  return buf;
}

describe("review frame packets (review-contract.json)", () => {
  const k = REVIEW_FRAME_PACKET.pull_kinds;
  const f = REVIEW_FRAME_PACKET.pixel_formats;

  it("decodes RGB8 thumbnails and Mono8 frames on the review pull kinds", () => {
    const rgb = decodeReviewPacket(packet(k.review_thumbnails, 2, 2, f.rgb8, 3), k.review_thumbnails);
    expect(rgb.pixel_format).toBe(f.rgb8);
    expect(rgb.stride_bytes).toBe(6);
    expect(rgb.data.length).toBe(12);
    const mono = decodeReviewPacket(packet(k.review, 4, 2, f.mono8, 1), k.review);
    expect(mono.frame_index).toBe("7");
    expect(mono.session_id).toBeNull();
  });

  it("rejects live pull kinds, unknown formats and capture identities", () => {
    expect(() => decodeReviewPacket(packet(1, 2, 2, f.mono8, 1), 1)).toThrow("FRAME_PACKET_INVALID_FLAGS_OR_SOURCE");
    expect(() => decodeReviewPacket(packet(k.review, 2, 2, 99, 1), k.review)).toThrow("FRAME_PACKET_INVALID_GEOMETRY_OR_FORMAT");
    expect(() => decodeReviewPacket(packet(k.review, 2, 2, f.rgb8, 1), k.review)).toThrow("FRAME_PACKET_INVALID_GEOMETRY_OR_FORMAT");
    expect(() => decodeReviewPacket(packet(k.review, 2, 2, f.mono8, 1, { 72: 5n }), k.review)).toThrow("FRAME_PACKET_UNSUPPORTED_IDENTITY");
    expect(() => decodeReviewPacket(packet(k.review_series, 2, 2, f.mono8, 1), k.review)).toThrow("FRAME_PACKET_INVALID_FLAGS_OR_SOURCE");
  });
});
