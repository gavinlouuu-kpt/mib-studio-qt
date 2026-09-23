import {decimalU64} from './framePacket';
export interface ProcessedPreview {
  valid: boolean; width: number; height: number; frame_index: string; source_timestamp: string;
  source_timestamp_unit: 'source_native_unknown'; host_timestamp_us: string;
  capture_session: string; capture_stale: boolean; processing_session: string; store_generation: string; recipe_sha256: string;
  primary_bounds: number[]; roi: number[]; contours: number[][][]; contours_truncated: boolean;
  primary_object_valid: boolean; primary_object_target: boolean;
  pixels: Uint8Array; mask: Uint8Array;
}
/** One atomic immutable packet, never metadata from a second latest-frame query. */
export function decodeProcessedPreview(buffer: ArrayBuffer): ProcessedPreview | null {
  const bytes = new Uint8Array(buffer), view = new DataView(buffer);
  if (bytes.length < 12 || String.fromCharCode(...bytes.subarray(0,4)) !== 'MIPO' || view.getUint32(4, true) !== 1) throw new Error('Invalid processed preview header');
  const jsonLength = view.getUint32(8, true);
  if (jsonLength > 1024 * 1024 || 12 + jsonLength > bytes.length) throw new Error('Invalid processed preview metadata length');
  const meta = JSON.parse(new TextDecoder('utf-8', {fatal: true}).decode(bytes.subarray(12, 12 + jsonLength)));
  if (meta.valid === false) {if (bytes.length !== 12 + jsonLength) throw new Error('Unexpected preview payload'); return null;}
  const count = meta.width * meta.height;
  if (meta.valid !== true || !Number.isSafeInteger(meta.width) || !Number.isSafeInteger(meta.height) || meta.width < 1 || meta.height < 1 || count > 32 * 1024 * 1024 || meta.image_bytes !== count || meta.mask_bytes !== count || bytes.length !== 12 + jsonLength + 2 * count) throw new Error('Invalid processed preview geometry');
  for (const name of ['frame_index', 'source_timestamp', 'host_timestamp_us', 'processing_session', 'store_generation', 'capture_session']) {if (typeof meta[name] !== 'string') throw new Error('Invalid processed preview identity'); decimalU64(meta[name]);}
  if (typeof meta.recipe_sha256 !== 'string' || !/^[0-9a-f]{64}$/.test(meta.recipe_sha256) || meta.source !== 'live_processing' || meta.source_timestamp_unit !== 'source_native_unknown') throw new Error('Invalid preview provenance');
  if (!Array.isArray(meta.roi) || meta.roi.length !== 4 || !meta.roi.every(Number.isFinite) || !Array.isArray(meta.contours) || meta.contours.length > 512) throw new Error('Invalid preview overlay');
  if (!Array.isArray(meta.primary_bounds) || meta.primary_bounds.length !== 4 || !meta.primary_bounds.every(Number.isFinite)) throw new Error('Invalid primary object bounds');
  let points = 0;
  for (const contour of meta.contours) {
    if (!Array.isArray(contour)) throw new Error('Invalid contour');
    points += contour.length;
    if (points > 20000 || contour.some((point: unknown) => !Array.isArray(point) || point.length !== 2 || !point.every(Number.isFinite))) throw new Error('Invalid contour coordinates');
  }
  for (const key of ['contours_truncated', 'primary_object_valid', 'primary_object_target', 'capture_stale']) if (typeof meta[key] !== 'boolean') throw new Error('Invalid preview flags');
  return {...meta, pixels: bytes.subarray(12 + jsonLength, 12 + jsonLength + count), mask: bytes.subarray(12 + jsonLength + count)};
}
