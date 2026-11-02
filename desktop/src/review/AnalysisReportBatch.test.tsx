// @vitest-environment jsdom
import {act} from "react";
import {createRoot} from "react-dom/client";
import {expect,it,vi} from "vitest";
import {AnalysisReportBatch} from "./AnalysisReportBatch";
Object.assign(globalThis,{IS_REACT_ACT_ENVIRONMENT:true});
it("stops after an in-flight report and sends cancellation using returned native revision",async()=>{
 const host=document.createElement("div"),root=createRoot(host);
 const response=(revision:string,state:string,next:number)=>({status:"ok",revision,document:{state,next,runs:[{},{}],results:[],failure:null}});
 const read=vi.fn().mockResolvedValue(response("1","ready",0));
 let resolveStep!:(r:Record<string,any>)=>void;
 const write=vi.fn().mockImplementationOnce(()=>new Promise(resolve=>{resolveStep=resolve;})).mockResolvedValue(response("3","cancelled",1));
 const button=(name:string)=>Array.from(host.querySelectorAll("button")).find(b=>b.textContent===name)!;
 try{
  await act(async()=>root.render(<AnalysisReportBatch read={read} write={write} disabled={false}/>));
  // Populate the ordinary controlled batch-ID input through React's native event path.
  const input=host.querySelector("input")!;
  await act(async()=>{Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,"value")!.set!.call(input,"batch");input.dispatchEvent(new Event("input",{bubbles:true}));});
  await act(async()=>{button("Run remaining reports").click();});
  expect(write).toHaveBeenCalledWith({action:"reportBatchAdvance",batchId:"batch",revision:"1"});
  await act(async()=>{button("Stop after current report").click();resolveStep(response("2","ready",1));});
  expect(write.mock.calls[1][0]).toEqual({action:"reportBatchCancel",batchId:"batch",revision:"2"});
  expect(write).toHaveBeenCalledTimes(2);expect(host.textContent).toContain("1/2 reports complete");expect(host.textContent).toContain("cancelled");
 }finally{await act(async()=>root.unmount());}
});
