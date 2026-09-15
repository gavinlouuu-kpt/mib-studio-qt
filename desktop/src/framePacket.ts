import { FRAME_PACKET } from "./bridgeContract";

export interface FrameMeta {
  valid: boolean;
  frame_index: string;
  /** Legacy raw field; clock, unit verification and freshness unavailable. */
  timestamp_ns: string;
  timestamp_validity: "unavailable";
  source_id: null;
  session_id: null;
  config_revision: null;
  pull_kind: number;
  width: number;
  height: number;
  pixel_format: number;
  stride_bytes: number;
  byte_len: number;
}
export interface FramePacket extends FrameMeta { readonly data: Uint8Array }

/** Exact unsigned identity at a JSON boundary; rejects already-rounded numbers. */
export function decimalU64(value: string | number | bigint): string {
  if (typeof value === "number" && (!Number.isSafeInteger(value) || value < 0))
    throw new Error("INVALID_U64: unsafe number");
  const s = String(value);
  if (!/^(0|[1-9][0-9]{0,19})$/.test(s) || BigInt(s) > 18446744073709551615n)
    throw new Error("INVALID_U64: expected canonical unsigned decimal");
  return s;
}

export function decodeFramePacket(buffer: ArrayBuffer, expectedKind: number): FramePacket {
  const c = FRAME_PACKET;
  if (!(buffer instanceof ArrayBuffer) || buffer.byteLength < c.header_bytes
      || buffer.byteLength > c.header_bytes + c.max_payload_bytes)
    throw new Error("FRAME_PACKET_INVALID_LENGTH");
  const d = new DataView(buffer);
  if (d.getUint32(0, true) !== 0x4642494d || d.getUint16(4, true) !== c.version
      || d.getUint16(6, true) !== c.header_bytes)
    throw new Error("FRAME_PACKET_INCOMPATIBLE_VERSION");
  const flags = d.getUint32(8, true), kind = d.getUint32(12, true);
  if (flags > 1 || kind !== expectedKind || kind < 1 || kind > 4)
    throw new Error("FRAME_PACKET_INVALID_FLAGS_OR_SOURCE");
  const n = (offset: number) => d.getBigUint64(offset, true);
  // v1 deliberately has no authoritative identity/time validity bits.
  if (n(72) !== 0n || n(80) !== 0n || n(88) !== 0n)
    throw new Error("FRAME_PACKET_UNSUPPORTED_IDENTITY");
  const width = n(32), height = n(40), format = n(48), stride = n(56), len = n(64);
  if (len !== BigInt(buffer.byteLength - c.header_bytes))
    throw new Error("FRAME_PACKET_INVALID_LENGTH");
  if (flags === 1) {
    if (width === 0n || height === 0n || width > BigInt(c.max_dimension)
        || height > BigInt(c.max_dimension) || width * height > BigInt(c.max_pixels)
        || stride < width || stride * height !== len
        || (format !== 0n && format !== 0x01080001n))
      throw new Error("FRAME_PACKET_INVALID_GEOMETRY_OR_FORMAT");
  } else if ([n(16), n(24), width, height, format, stride, len].some(v => v !== 0n)) {
    throw new Error("FRAME_PACKET_INVALID_EMPTY_FRAME");
  }
  return Object.freeze({
    valid: flags === 1, frame_index: n(16).toString(), timestamp_ns: n(24).toString(),
    timestamp_validity: "unavailable", source_id: null, session_id: null,
    config_revision: null, pull_kind: kind, width: Number(width), height: Number(height),
    pixel_format: Number(format), stride_bytes: Number(stride), byte_len: Number(len),
    // This view owns its response buffer. Never place it in global React state.
    data: new Uint8Array(buffer, c.header_bytes),
  });
}
