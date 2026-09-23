// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { beforeEach, afterEach, expect, it, vi } from "vitest";
import { bridge } from "./bridge";
import { useReviewExport, ExportStatus } from "./exportControls";
import { open } from "@tauri-apps/plugin-dialog";
vi.mock("./bridge",()=>({bridge:{reviewExport:vi.fn(),reviewExportStatus:vi.fn(),cancelOperation:vi.fn()}}));
vi.mock("@tauri-apps/plugin-dialog",()=>({open:vi.fn(),save:vi.fn()}));
Object.assign(globalThis,{IS_REACT_ACT_ENVIRONMENT:true});
let root:Root,host:HTMLDivElement, model:ReturnType<typeof useReviewExport>, visible:boolean;
function Harness(){model=useReviewExport(true,vi.fn());return visible ? <ExportStatus model={model}/> : <p>Other tab</p>;}
const running={state:"running" as const,operation_id:"9007199254740993",phase:"valid_images",completed:"3",total:"12"};
beforeEach(async()=>{
  vi.useFakeTimers();vi.resetAllMocks();visible=true;
  vi.mocked(open).mockResolvedValue("/tmp/export");
  vi.mocked(bridge.reviewExportStatus).mockResolvedValue({state:"idle"});
  vi.mocked(bridge.reviewExport).mockResolvedValue({ok:true,operation_id:running.operation_id,message:"accepted"} as never);
  vi.mocked(bridge.cancelOperation).mockResolvedValue({ok:true,message:"cancel requested"} as never);
  host=document.createElement("div");document.body.append(host);root=createRoot(host);
  await act(async()=>root.render(<Harness/>));
});
afterEach(async()=>{await act(async()=>root.unmount());host.remove();vi.useRealTimers();});
it("does not treat cancellation acceptance as completed cancellation",async()=>{
  vi.mocked(bridge.reviewExportStatus).mockResolvedValue(running);
  await act(async()=>{await model.start("all",0.5);});
  await act(async()=>{await model.cancel();});
  expect(bridge.cancelOperation).toHaveBeenCalledWith("9007199254740993");
  expect(model.status.state).toBe("running");
  vi.mocked(bridge.reviewExportStatus).mockResolvedValue({...running,state:"cancelled",retained_partial_path:"/tmp/.run.partial-1"});
  await act(async()=>{await vi.advanceTimersByTimeAsync(500);});
  expect(host.textContent).toContain("Incomplete output retained: /tmp/.run.partial-1");
});
it("retains and reconciles a native job when navigating away",async()=>{
  vi.mocked(bridge.reviewExportStatus).mockResolvedValue(running);
  await act(async()=>{await model.start("images",0.5);});
  visible=false;await act(async()=>root.render(<Harness/>));
  vi.mocked(bridge.reviewExportStatus).mockResolvedValue({...running,state:"completed",final_path:"/tmp/export/run"});
  await act(async()=>{await vi.advanceTimersByTimeAsync(500);});
  visible=true;await act(async()=>root.render(<Harness/>));
  expect(model.status.state).toBe("completed");expect(host.textContent).toContain("/tmp/export/run");
  expect(bridge.cancelOperation).not.toHaveBeenCalled();
});
it("bounds pending submissions and never clears command rejection on a good status poll",async()=>{
  let reject!:(reason:Error)=>void;
  vi.mocked(bridge.reviewExport).mockReturnValue(new Promise((_,r)=>{reject=r;}));
  let command!:Promise<void>;
  await act(async()=>{command=model.start("all",1);await Promise.resolve();});
  await act(async()=>{await model.start("all",1);});
  expect(bridge.reviewExport).toHaveBeenCalledOnce();
  await act(async()=>{reject(new Error("Export destination is invalid"));await command;});
  await act(async()=>{await vi.advanceTimersByTimeAsync(500);});
  expect(host.textContent).toContain("Export destination is invalid");
});
it("never overlaps status polls under a slow native response",async()=>{
  vi.mocked(bridge.reviewExportStatus).mockClear();
  let finish!:(value:typeof running)=>void;
  vi.mocked(bridge.reviewExportStatus).mockReturnValue(new Promise(r=>{finish=r;}));
  await act(async()=>{await vi.advanceTimersByTimeAsync(5000);});
  expect(bridge.reviewExportStatus).toHaveBeenCalledOnce();
  await act(async()=>{finish(running);});
});

it("submits batch sources once without replacing the open review file",async()=>{
  vi.mocked(open).mockResolvedValueOnce(["/data/a.h5","/data/b.h5"]).mockResolvedValueOnce("/tmp/out");
  await act(async()=>{await model.start("metrics_csv",undefined,true);});
  expect(bridge.reviewExport).toHaveBeenCalledWith(expect.objectContaining({source_paths:["/data/a.h5","/data/b.h5"],output_root:"/tmp/out",format:"metrics_csv"}));
  expect(vi.mocked(bridge.reviewExport).mock.calls[0][0]).not.toHaveProperty("explicit_destination");
});
it("cancelled batch selection submits nothing",async()=>{
  vi.mocked(open).mockResolvedValueOnce(null);
  await act(async()=>{await model.start("all",undefined,true);});
  expect(bridge.reviewExport).not.toHaveBeenCalled();
});
