import { expect, it } from "vitest";
import { decodeEvents, decodeCommandResult, decodeExperimentStatus } from "./eventAdapter";
import golden from "../../crates/mib-bridge/contract/fixtures/events-v1.json";
import { JSON_TRANSPORT } from "./bridgeContract";
const fixture=()=>structuredClone(golden);
it("decodes the shared C++/Rust JSON golden into named fields",()=>{
  const events=decodeEvents(fixture());
  expect(events[0]).toMatchObject({kind:"OperationStatus",operationId:"9007199254740993",state:2,progress:"18446744073709551615",total:"18446744073709551615"});
  expect(events[1]).toMatchObject({kind:"ExperimentStatus",endTimeNs:"18446744073709551615",droppedValid:"9007199254740993",droppedInvalid:"18446744073709551615"});
  expect(events[2]).toMatchObject({algorithmFps:{value:null,validity:"invalid",freshness:"unavailable"},validFps:{value:0,validity:"reported",freshness:"unavailable"},invalidFps:{value:null,validity:"invalid"}});
  expect(events[3]).toMatchObject({kind:"FrameReady",source:2,byteSize:"4"});
  for(const e of events) expect(e).not.toHaveProperty("u0");
});
it.each(["", "-1", "+1", "01", "1.0", "1e3", "18446744073709551616", 9007199254740992])("rejects malformed/rounded operation ID %s",value=>{
  const f=fixture(); (f.events[0] as Record<string,unknown>).u0=value;
  expect(()=>decodeEvents(f)).toThrow();
});
it("ignores optional fields, preserves unknown event notice, refuses unknown state",()=>{
  const f=fixture(); (f as Record<string,unknown>).extra={safe:true};
  (f.events[0] as Record<string,unknown>).extra=42; expect(decodeEvents(f)).toHaveLength(4);
  f.events[0].kind="FutureOptionalNotice";
  expect(decodeEvents(f)[0]).toEqual({kind:"Unknown",receivedKind:"FutureOptionalNotice",reconciliationRequired:true});
  const bad=fixture(); bad.events[0].u2="99";
  expect(()=>decodeEvents(bad)).toThrow("TRANSPORT_UNKNOWN_REQUIRED_ENUM");
});
it("rejects incompatible versions and bounded payload violations",()=>{
  expect(()=>decodeEvents({...fixture(),transport_version:2})).toThrow("TRANSPORT_INCOMPATIBLE_VERSION");
  expect(()=>decodeEvents({...fixture(),events:Array(JSON_TRANSPORT.max_events+1).fill(golden.events[0])})).toThrow("TRANSPORT_EVENT_COUNT_LIMIT");
  const f=fixture(); f.events[0].text="x".repeat(JSON_TRANSPORT.max_event_text_bytes+1);
  expect(()=>decodeEvents(f)).toThrow("TRANSPORT_EVENT_TEXT_LIMIT");
});
it("distinguishes command acceptance from a terminal operation notification",()=>{
  const accepted=decodeCommandResult({transport_version:1,ok:true,command:6,message:"accepted",operation_id:"9007199254740993"});
  expect(accepted).toEqual({ok:true,command:6,message:"accepted",operation_id:"9007199254740993"});
  expect(accepted).not.toHaveProperty("completed");
  expect(()=>decodeCommandResult({...accepted,transport_version:1,operation_id:9007199254740992})).toThrow();
});
it("keeps experiment snapshot counts exact and refuses partial snapshots",()=>{
  const snapshot={transport_version:1,valid:true,state:2,start_time_ns:"18446744073709551614",end_time_ns:"18446744073709551615",valid_buffered:"9007199254740993",invalid_buffered:"5",valid_saved:"18446744073709551615",invalid_saved:"7",dropped_valid:"9007199254740993",dropped_invalid:"18446744073709551615",flushing:true,cancelled:false,output_path:"test.h5",message:"saving"};
  expect(decodeExperimentStatus(snapshot).valid_saved).toBe("18446744073709551615");
  expect(()=>decodeExperimentStatus({...snapshot,dropped_invalid:undefined})).toThrow();
});

import {decodeProcessingStats,formatMetric} from "./eventAdapter";
it("does not invent zero or freshness for unavailable/invalid processing samples",()=>{
  const s={transport_version:1,valid:true,algo_fps1s:0,valid_fps1s:null,invalid_fps1s:Infinity,pixel_to_micron:NaN};
  const decoded=decodeProcessingStats(s);
  expect(decoded).toMatchObject({algo_fps1s:0,valid_fps1s:null,invalid_fps1s:null,pixel_to_micron:null,freshness:"unavailable"});
  expect(formatMetric(decoded.algo_fps1s)).toBe("0.0");
  expect(formatMetric(decoded.valid_fps1s)).toBe("Unavailable");
  expect(decodeProcessingStats({...s,valid:false}).algo_fps1s).toBeNull();
});
