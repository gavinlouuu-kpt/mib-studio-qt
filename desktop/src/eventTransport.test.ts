import { beforeEach, expect, it, vi } from "vitest";
import { bridge } from "./bridge";
const { invoke } = vi.hoisted(() => ({invoke:vi.fn()}));
vi.mock("@tauri-apps/api/core",()=>({invoke}));
beforeEach(()=>{invoke.mockReset();});
it("preserves operation identity above the JSON safe-integer limit",async()=>{
  invoke.mockImplementation(async (command:string)=>{
    const e={kind:"OperationStatus",u0:"9007199254740993",u1:"1",u2:"2",u3:"18446744073709551615",u4:"18446744073709551615",u5:"0",f0:0,f1:0,f2:0,b0:false,b1:false,text:"complete",
      experiment_end_time_ns:"0",experiment_dropped_valid:"0",experiment_dropped_invalid:"0",frame_byte_size:"0"};
    if(command==="poll_events_exact") return {transport_version:1,events:[e]};
    return [{...e,u0:Number(e.u0),u1:1,u2:2,u3:Number(e.u3),u4:Number(e.u4),u5:0}];
  });
  const events=await bridge.pollEvents();
  const e=events[0];
  const id="operationId" in e ? e.operationId : "u0" in e ? String(e.u0) : null;
  expect(id).toBe("9007199254740993");
});
it("sends exact cancellation and seek IDs without numeric conversion",async()=>{
  invoke.mockResolvedValue({transport_version:1,ok:true,command:5,message:"accepted",operation_id:"18446744073709551615"});
  await bridge.cancelOperation("18446744073709551615");
  expect(invoke).toHaveBeenCalledWith("cancel_operation",{operationId:"18446744073709551615"});
  await bridge.seekIndex("9007199254740993");
  expect(invoke).toHaveBeenCalledWith("seek_index",{frameIndex:"9007199254740993"});
  expect(()=>bridge.cancelOperation(9007199254740992)).toThrow("INVALID_U64");
});
