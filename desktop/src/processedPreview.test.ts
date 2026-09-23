import {describe, it, expect} from 'vitest';
import {decodeProcessedPreview} from './processedPreview';
function packet(overrides = {}, payload = [11,22,0,255]) {
  const meta = {valid: true, width: 2, height: 1, image_bytes: 2, mask_bytes: 2, source: 'live_processing', frame_index: '18446744073709551614', source_timestamp: '42', source_timestamp_unit: 'source_native_unknown', host_timestamp_us: '0', capture_session: '9007199254740993', capture_stale: false, processing_session: '4', store_generation: '9', recipe_sha256: 'a'.repeat(64), primary_bounds: [0,0,1,1], roi: [0,0,2,1], contours: [[[0,0],[1,0]]], contours_truncated: false, primary_object_valid: true, primary_object_target: false, ...overrides};
  const json = new TextEncoder().encode(JSON.stringify(meta)); const bytes = new Uint8Array(12+json.length+payload.length); bytes.set([77,73,80,79,1,0,0,0]); new DataView(bytes.buffer).setUint32(8,json.length,true); bytes.set(json,12); bytes.set(payload,12+json.length); return bytes.buffer;
}
describe('atomic processed preview', () => {
  it('keeps exact u64 identities and paired source/mask bytes', () => {const value = decodeProcessedPreview(packet())!; expect(value.frame_index).toBe('18446744073709551614'); expect([...value.pixels]).toEqual([11,22]); expect([...value.mask]).toEqual([0,255]);});
  it('rejects truncated, trailing and mismatched geometry rather than painting a partial frame', () => {expect(() => decodeProcessedPreview(packet({},[1,2]))).toThrow(); expect(() => decodeProcessedPreview(packet({},[1,2,3,4,5]))).toThrow(); expect(() => decodeProcessedPreview(packet({width: 4}))).toThrow();});
  it('rejects unbounded and malformed contours or unknown provenance', () => {expect(() => decodeProcessedPreview(packet({contours: [[['x',0]]]}))).toThrow(); expect(() => decodeProcessedPreview(packet({source: 'unknown'}))).toThrow(); expect(() => decodeProcessedPreview(packet({recipe_sha256: ''}))).toThrow();});
  it('handles explicitly unavailable snapshot without stale payload', () => {expect(decodeProcessedPreview(packet({valid: false, image_bytes: 0, mask_bytes: 0},[]))).toBeNull(); expect(() => decodeProcessedPreview(packet({valid: false}))).toThrow();});
});
