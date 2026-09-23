import {beforeEach,it,expect,vi} from "vitest";
import {bridge} from "./bridge";
import {invoke} from "@tauri-apps/api/core";
import {recoverNativeRuntime} from "./runtimeRecovery";
vi.mock("./bridge",()=>({bridge:{isInitialized:vi.fn(),init:vi.fn(),fetchExperimentStatus:vi.fn(),fetchReviewMetadata:vi.fn()}}));
vi.mock("@tauri-apps/api/core",()=>({invoke:vi.fn()}));
beforeEach(()=>{vi.resetAllMocks();vi.mocked(bridge.isInitialized).mockResolvedValue(true);vi.mocked(invoke).mockResolvedValue({capture_running:false,recording:false});vi.mocked(bridge.fetchExperimentStatus).mockResolvedValue({state:0} as never);vi.mocked(bridge.fetchReviewMetadata).mockResolvedValue({file_open:true,recording_file:true,file_path:"retained.h5"} as never);});
it("resumes native capture, recording, experiment and open review without initialization",async()=>{const value=await recoverNativeRuntime();expect(value).toMatchObject({resumed:true,runtime:{capture_running:false,recording:false},experiment:{state:0},review:{file_path:"retained.h5"}});expect(bridge.init).not.toHaveBeenCalled();});
it("does not publish readiness while authoritative experiment state is pending",async()=>{let resolve!:(v:never)=>void;vi.mocked(bridge.fetchExperimentStatus).mockImplementation(()=>new Promise(r=>{resolve=r;}));let completed=false;const pending=recoverNativeRuntime().then(v=>{completed=true;return v;});await vi.waitFor(()=>expect(invoke).toHaveBeenCalled());expect(completed).toBe(false);resolve({state:2} as never);expect((await pending).resumed).toBe(true);});
it("initializes only a genuinely fresh backend and fails closed on status faults",async()=>{vi.mocked(bridge.isInitialized).mockResolvedValue(false);vi.mocked(bridge.init).mockResolvedValue(true);vi.mocked(bridge.fetchReviewMetadata).mockRejectedValue(new Error("status unavailable"));await expect(recoverNativeRuntime()).rejects.toThrow("status unavailable");expect(bridge.init).toHaveBeenCalledWith("");});

it.each([{capture_running:true,recording:true,state:0},{capture_running:true,recording:false,state:2}])("does not recover shared writer handles as review sources: %j",async({capture_running,recording,state})=>{
 vi.mocked(invoke).mockResolvedValue({capture_running,recording});vi.mocked(bridge.fetchExperimentStatus).mockResolvedValue({state} as never);
 const result=await recoverNativeRuntime();expect(result.review.file_open).toBe(false);expect(result.review.file_path).toBe("");expect(result.runtime.recording).toBe(recording);
});
it("an open HDF handle without a loaded review path is not a review session",async()=>{vi.mocked(bridge.fetchReviewMetadata).mockResolvedValue({file_open:true,file_path:""} as never);expect((await recoverNativeRuntime()).review.file_open).toBe(false);});
