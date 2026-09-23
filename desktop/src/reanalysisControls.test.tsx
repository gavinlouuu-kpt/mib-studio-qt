// @vitest-environment jsdom
import {act} from "react";
import {createRoot,type Root} from "react-dom/client";
import {beforeEach,afterEach,it,expect,vi} from "vitest";
import {useReanalysis,ReanalysisStatus,ReanalysisControls} from "./reanalysisControls";
import {bridge} from "./bridge";
import {save} from "@tauri-apps/plugin-dialog";
vi.mock("./bridge",()=>({bridge:{reviewReanalysis:vi.fn(),reviewReanalysisStatus:vi.fn(),fetchReviewMetadata:vi.fn(),cancelOperation:vi.fn(),fetchReanalysisPreview:vi.fn()},mono8ToImageData:vi.fn()}));
vi.mock("@tauri-apps/plugin-dialog",()=>({save:vi.fn()}));
Object.assign(globalThis,{IS_REACT_ACT_ENVIRONMENT:true});
let showControls=false;
let root:Root,host:HTMLDivElement,model:ReturnType<typeof useReanalysis>;
function Harness(){model=useReanalysis(true);return <><ReanalysisStatus model={model}/>{showControls && <ReanalysisControls model={model} metadata={{file_open:true,file_path:"source.h5"} as never}/>}</>;}
beforeEach(async()=>{vi.useFakeTimers();vi.resetAllMocks();showControls=false;host=document.createElement("div");document.body.append(host);root=createRoot(host);vi.mocked(save).mockResolvedValue("new.h5");vi.mocked(bridge.fetchReviewMetadata).mockResolvedValue({file_open:true,file_path:"source.h5"} as never);vi.mocked(bridge.reviewReanalysisStatus).mockResolvedValue({state:"idle"});vi.mocked(bridge.reviewReanalysis).mockResolvedValue({ok:true,operation_id:"12"} as never);await act(async()=>root.render(<Harness/>));});
afterEach(async()=>{await act(async()=>root.unmount());host.remove();vi.useRealTimers();});
it("validates source again after choosing output and leaves source untouched",async()=>{
  vi.mocked(bridge.fetchReviewMetadata).mockResolvedValue({file_open:true,file_path:"other.h5"} as never);
  await act(async()=>model.start("source.h5","/valid_frames/images",0,10));
  expect(bridge.reviewReanalysis).not.toHaveBeenCalled();expect(host.textContent).toContain("source changed");
});
it("prevents duplicate submission and waits for native cancellation result",async()=>{
  await act(async()=>model.start("source.h5","/valid_frames/images",0,10));
  await act(async()=>model.start("source.h5","/valid_frames/images",0,10));
  expect(bridge.reviewReanalysis).toHaveBeenCalledOnce();
  vi.mocked(bridge.cancelOperation).mockResolvedValue({ok:true} as never);
  await act(async()=>model.cancel());expect(model.status.state).toBe("running");
  vi.mocked(bridge.reviewReanalysisStatus).mockResolvedValue({state:"cancelled",operation_id:"12"});
  await act(async()=>vi.advanceTimersByTimeAsync(500));expect(model.status.state).toBe("cancelled");
});
it("does not publish a failed backend job as a successful save",async()=>{
  await act(async()=>model.start("source.h5","/valid_frames/images",0,10));
  vi.mocked(bridge.reviewReanalysisStatus).mockResolvedValue({state:"failed",operation_id:"12",error:"Disk write failed"});
  await act(async()=>vi.advanceTimersByTimeAsync(500));expect(host.textContent).toContain("Disk write failed");expect(host.textContent).not.toContain("Output:");
});

it("accepts an independent image folder without replacing or requiring the open HDF reader",async()=>{
  vi.mocked(bridge.fetchReviewMetadata).mockClear();
  await act(async()=>model.start("/images","all",1,3,{source_kind:"folder",synthetic_background:true,roi:{x:0,y:0,w:10,h:10}}));
  expect(bridge.fetchReviewMetadata).not.toHaveBeenCalled();
  expect(bridge.reviewReanalysis).toHaveBeenCalledWith(expect.objectContaining({source_path:"/images",source_kind:"folder",start:1,count:3,synthetic_background:true}));
});

it("preserves local processing drafts across leaving and returning to Review",async()=>{
  showControls=true;await act(async()=>root.render(<Harness/>));
  const field=host.querySelector<HTMLTextAreaElement>('textarea')!;
  await act(async()=>{Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype,"value")!.set!.call(field,'{"threshold":17}');field.dispatchEvent(new Event("input",{bubbles:true}));});
  showControls=false;await act(async()=>root.render(<Harness/>));
  showControls=true;await act(async()=>root.render(<Harness/>));
  expect(host.querySelector<HTMLTextAreaElement>('textarea')!.value).toBe('{"threshold":17}');
});
it("does not offer a late preview as background for a different requested index",async()=>{
  showControls=true;await act(async()=>root.render(<Harness/>));
  let complete!:(value:never)=>void;vi.mocked(bridge.fetchReanalysisPreview).mockReturnValue(new Promise(resolve=>{complete=resolve;}));
  let pending!:Promise<void>;
  await act(async()=>{pending=model.loadPreview({source_kind:"hdf",source_path:"source.h5",dataset:"/valid_frames/images",index:0});});
  await act(async()=>model.draft.setPreviewIndex("1"));
  await act(async()=>{complete({valid:true,width:1,height:1,data:new Uint8Array([42])} as never);await pending;});
  const button=Array.from(host.querySelectorAll("button")).find(b=>b.textContent==="Use preview frame as background")!;
  expect(button.disabled).toBe(true);
});
it("reload recovers a native reanalysis and blocks duplicate work until reconciliation",async()=>{
 await act(async()=>root.unmount());let finish!:(v:never)=>void;
 vi.mocked(bridge.reviewReanalysisStatus).mockImplementation(()=>new Promise(r=>{finish=r;}));root=createRoot(host);
 await act(async()=>root.render(<Harness/>));expect(model.busy).toBe(true);
 await act(async()=>model.start("source.h5","all",0,0));expect(bridge.reviewReanalysis).not.toHaveBeenCalled();
 await act(async()=>finish({state:"running",operation_id:"9007199254740993"} as never));
 vi.mocked(bridge.cancelOperation).mockResolvedValue({ok:true} as never);
 await act(async()=>model.cancel());expect(bridge.cancelOperation).toHaveBeenCalledWith("9007199254740993");
});
