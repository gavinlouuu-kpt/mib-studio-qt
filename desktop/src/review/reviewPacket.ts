// YOFO Review frame packets (ADR 0014). The header layout, magic, version and
// limits are the bridge contract's (FRAME_PACKET in ../bridgeContract);
// review-contract.json adds the review pull kinds (3, 5, 6) and RGB8 for
// backend-composed overlays. Capture identities are always zero: review
// frames come from a recorded file. MIB Studio's ../framePacket.ts decoder is
// separate and unchanged.
import { FRAME_PACKET } from "../bridgeContract";
import type { FramePacket } from "../framePacket";
import { REVIEW_FRAME_PACKET } from "./reviewContract";

const REVIEW_KINDS: readonly number[] = Object.values(REVIEW_FRAME_PACKET.pull_kinds);

/** Bytes per pixel of a review pixel format; 0 when unknown. */
function bytesPerPixel(format: bigint): bigint {
  const f = REVIEW_FRAME_PACKET.pixel_formats;
  if (format === BigInt(f.mono8_legacy) || format === BigInt(f.mono8)) return 1n;
  if (format === BigInt(f.rgb8)) return 3n;
  return 0n;
}

export function decodeReviewPacket(buffer: ArrayBuffer, expectedKind: number): FramePacket {
  const c = FRAME_PACKET;
  if (!(buffer instanceof ArrayBuffer) || buffer.byteLength < c.header_bytes
      || buffer.byteLength > c.header_bytes + c.max_payload_bytes)
    throw new Error("FRAME_PACKET_INVALID_LENGTH");
  const d = new DataView(buffer);
  if (d.getUint32(0, true) !== 0x4642494d || d.getUint16(4, true) !== c.version
      || d.getUint16(6, true) !== c.header_bytes)
    throw new Error("FRAME_PACKET_INCOMPATIBLE_VERSION");
  const flags = d.getUint32(8, true), kind = d.getUint32(12, true);
  if (flags > 1 || kind !== expectedKind || !REVIEW_KINDS.includes(kind))
    throw new Error("FRAME_PACKET_INVALID_FLAGS_OR_SOURCE");
  const n = (offset: number) => d.getBigUint64(offset, true);
  if (n(72) !== 0n || n(80) !== 0n || n(88) !== 0n)
    throw new Error("FRAME_PACKET_UNSUPPORTED_IDENTITY");
  const width = n(32), height = n(40), format = n(48), stride = n(56), len = n(64);
  if (len !== BigInt(buffer.byteLength - c.header_bytes))
    throw new Error("FRAME_PACKET_INVALID_LENGTH");
  if (flags === 1) {
    const bpp = bytesPerPixel(format);
    if (width === 0n || height === 0n || width > BigInt(c.max_dimension)
        || height > BigInt(c.max_dimension) || width * height > BigInt(c.max_pixels)
        || bpp === 0n || stride < width * bpp || stride * height !== len)
      throw new Error("FRAME_PACKET_INVALID_GEOMETRY_OR_FORMAT");
  } else if ([n(16), n(24), width, height, format, stride, len].some((v) => v !== 0n)) {
    throw new Error("FRAME_PACKET_INVALID_EMPTY_FRAME");
  }
  return Object.freeze({
    valid: flags === 1, frame_index: n(16).toString(), timestamp_ns: n(24).toString(),
    timestamp_validity: "unavailable", source_id: null, session_id: null, store_generation: null,
    config_revision: null, pull_kind: kind, width: Number(width), height: Number(height),
    pixel_format: Number(format), stride_bytes: Number(stride), byte_len: Number(len),
    // This view owns its response buffer. Never place it in global React state.
    data: new Uint8Array(buffer, c.header_bytes),
  });
}
