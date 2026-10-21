import golden from "../../crates/mib-bridge/contract/fixtures/frame-v2.json";
import cellGolden from "../../crates/mib-bridge/contract/fixtures/monitoring-cell-v2.json";
import { expect, it } from "vitest";
import { decodeFramePacket, decimalU64 } from "./framePacket";
import { FRAME_PACKET } from "./bridgeContract";
function fixture(id = 18446744073709551615n) {
  const b = new ArrayBuffer(100), d = new DataView(b);
  d.setUint32(0,0x4642494d,true); d.setUint16(4,2,true); d.setUint16(6,96,true);
  d.setUint32(8,1,true); d.setUint32(12,1,true);
  [id,id,2n,2n,0n,2n,4n,0n,0n,0n].forEach((v,i)=>d.setBigUint64(16+8*i,v,true));
  new Uint8Array(b,96).set([1,2,3,4]); return b;
}
it.each([9007199254740991n,9007199254740992n,9007199254740993n,18446744073709551615n])("decodes identity %s exactly", id=>{
  const p=decodeFramePacket(fixture(id),1);
  expect(p.frame_index).toBe(String(id)); expect(p.timestamp_ns).toBe(String(id));
  expect(p.timestamp_validity).toBe("unavailable"); expect(p.session_id).toBeNull();
  expect(p.config_revision).toBeNull(); expect(Array.from(p.data)).toEqual([1,2,3,4]);
});
it.each(["-1","01","+1","1.0","1e3",""," 1","18446744073709551616",9007199254740992,NaN,Infinity])("rejects malformed identity %s",value=>{
  expect(()=>decimalU64(value)).toThrow("INVALID_U64");
});
it("rejects truncation, excess bytes, oversize input and source mismatch",()=>{
  for(let i=0;i<100;i++) expect(()=>decodeFramePacket(fixture().slice(0,i),1)).toThrow();
  expect(()=>decodeFramePacket(new ArrayBuffer(FRAME_PACKET.header_bytes+FRAME_PACKET.max_payload_bytes+1),1)).toThrow();
  expect(()=>decodeFramePacket(fixture(),2)).toThrow();
  expect(()=>decodeFramePacket(new Uint8Array([...new Uint8Array(fixture()),0]).buffer,1)).toThrow();
});
it.each([32,40,48,56,64,80])("rejects malformed u64 field %s",offset=>{
  const b=fixture(); new DataView(b).setBigUint64(offset,18446744073709551615n,true);
  expect(()=>decodeFramePacket(b,1)).toThrow();
});
it("rejects incompatible headers and invalid empty frame",()=>{
  for(const offset of [0,4,6,8]) { const b=fixture(); new Uint8Array(b)[offset]=255; expect(()=>decodeFramePacket(b,1)).toThrow(); }
  const b=fixture(); new DataView(b).setUint32(8,0,true); expect(()=>decodeFramePacket(b,1)).toThrow();
});
it("accepts unavailable frame without inventing pixels",()=>{
  const b=fixture().slice(0,96); new DataView(b).setUint32(8,0,true); new Uint8Array(b,16).fill(0);
  expect(decodeFramePacket(b,1).valid).toBe(false); expect(decodeFramePacket(b,1).data.length).toBe(0);
});

it("decodes the binary golden also emitted by the C++/Rust producer test",()=>{
  const bytes=Uint8Array.from(golden.hex.match(/../g)!,h=>parseInt(h,16));
  const packet=decodeFramePacket(bytes.buffer,1);
  expect(packet.frame_index).toBe(golden.frame_index);
  expect(packet.timestamp_ns).toBe(golden.timestamp_ns);
  expect(Array.from(packet.data)).toEqual(golden.pixels);
});
it('carries exact acquisition/store epochs only for live raw frames',()=>{
 const b=fixture();const d=new DataView(b);d.setBigUint64(72,9007199254740993n,true);d.setBigUint64(88,18446744073709551615n,true);
 const p=decodeFramePacket(b,1);expect(p.session_id).toBe('9007199254740993');expect(p.store_generation).toBe('18446744073709551615');expect(p.config_revision).toBeNull();
 d.setUint32(12,3,true);expect(()=>decodeFramePacket(b,3)).toThrow('UNSUPPORTED_IDENTITY');
});
// Bridge ABI 41 (#716 A6a): a monitoring cell crop (pull kind 5) and its mask (6), the golden the
// Rust encoder test also produces. Backend-owned copies: no capture identities.
it("decodes the monitoring cell golden and keeps the kinds apart",()=>{
  const bytes=()=>Uint8Array.from(cellGolden.hex.match(/../g)!,h=>parseInt(h,16));
  expect(FRAME_PACKET.pull_kinds).toMatchObject({monitoring_cell_image:5,monitoring_cell_mask:6});
  const p=decodeFramePacket(bytes().buffer,FRAME_PACKET.pull_kinds.monitoring_cell_image);
  expect([p.pull_kind,p.frame_index,p.timestamp_ns,p.width,p.height]).toEqual([5,cellGolden.frame_index,cellGolden.timestamp_ns,cellGolden.width,cellGolden.height]);
  expect(Array.from(p.data)).toEqual(cellGolden.pixels);
  expect(p.session_id).toBeNull(); expect(p.store_generation).toBeNull();
  expect(()=>decodeFramePacket(bytes().buffer,6)).toThrow("FRAME_PACKET_INVALID_FLAGS_OR_SOURCE");
  const mask=bytes(); new DataView(mask.buffer).setUint32(12,6,true);
  expect(decodeFramePacket(mask.buffer,6).pull_kind).toBe(6);
  const unknown=bytes(); new DataView(unknown.buffer).setUint32(12,7,true);
  expect(()=>decodeFramePacket(unknown.buffer,7)).toThrow("FRAME_PACKET_INVALID_FLAGS_OR_SOURCE");
  const identity=bytes(); new DataView(identity.buffer).setBigUint64(72,1n,true);
  expect(()=>decodeFramePacket(identity.buffer,5)).toThrow("UNSUPPORTED_IDENTITY");
});
