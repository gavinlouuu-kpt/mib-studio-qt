// @vitest-environment jsdom
import {act} from "react";
import {createRoot,type Root} from "react-dom/client";
import {beforeEach,afterEach,it,expect,vi} from "vitest";
import {useReanalysis,ReanalysisStatus} from "./reanalysisControls";
import {bridge} from "./bridge";
import {save} from "@tauri-apps/plugin-dialog";
vi.mock("./bridge",()=>({bridge:{reviewReanalysis:vi.fn(),reviewReanalysisStatus:vi.fn(),fetchReviewMetadata:vi.fn(),cancelOperation:vi.fn()}}));
vi.mock("@tauri-apps/plugin-dialog",()=>({save:vi.fn()}));
Object.assign(globalThis,{IS_REACT_ACT_ENVIRONMENT:true});
let root:Root,host:HTMLDivElement,model:ReturnType<typeof useReanalysis>;
function Harness(){model=useReanalysis(true);return <ReanalysisStatus model={model}/>;}
beforeEach(async()=>{vi.useFakeTimers();vi.resetAllMocks();host=document.createElement("div");document.body.append(host);root=createRoot(host);vi.mocked(save).mockResolvedValue("new.h5");vi.mocked(bridge.fetchReviewMetadata).mockResolvedValue({file_open:true,file_path:"source.h5"} as never);vi.mocked(bridge.reviewReanalysisStatus).mockResolvedValue({state:"idle"});vi.mocked(bridge.reviewReanalysis).mockResolvedValue({ok:true,operation_id:"12"} as never);await act(async()=>root.render(<Harness/>));});
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
