// @vitest-environment jsdom
import {act} from "react";
import {createRoot,Root} from "react-dom/client";
import {beforeEach,afterEach,describe,it,expect,vi} from "vitest";
import {invoke} from "@tauri-apps/api/core";
import {open} from "@tauri-apps/plugin-dialog";
import {bridge} from "./bridge";
import {CameraDocumentEditor,useCameraDocument,type PzDocumentContext} from "./cameraDocument";
import type {PzCameraCheck,RunFpsStatus} from "./runFps";
vi.mock("@tauri-apps/api/core",()=>({invoke:vi.fn()}));vi.mock("@tauri-apps/plugin-dialog",()=>({open:vi.fn(),save:vi.fn()}));
vi.mock("./bridge",()=>({bridge:{checkPzCameraDocument:vi.fn(),applyPzCameraDocument:vi.fn(),fetchCameraSelection:vi.fn()}}));
Object.assign(globalThis,{IS_REACT_ACT_ENVIRONMENT:true});

let root:Root,host:HTMLDivElement,model:ReturnType<typeof useCameraDocument>,pz:PzDocumentContext,experimentActive:boolean;
const append=vi.fn(),refresh=vi.fn();
const timing={fps:1000,exposure_mode:"auto" as const,exposure_us:949.87,hmax:58,vmax:1280,shs:64,exposure_lines:1216,actual_fps:1000.135,exposure_start_us:50,exposure_end_us:1000,strobe_start_us:7,strobe_end_us:67,
  strobe_overlaps:true,strobe_straddles_exposure_start:true,strobe_warning:"",allowed_exposure_min_us:933,allowed_exposure_max_us:993};
const good:PzCameraCheck={ok:true,qualified:false,...timing};
const status=(over:Partial<RunFpsStatus>={}):RunFpsStatus=>({available:true,min_fps:1000,max_fps:5000,default_fps:5000,qualified_fps:5000,in_run:true,held:false,...over});
function Harness(){
 model=useCameraDocument({ready:true,running:false,experimentActive,selection:null,append,refresh},"pz7035",false,pz);
 return <CameraDocumentEditor model={model}/>;
}
const flush=async()=>{await act(async()=>{await new Promise(r=>setTimeout(r,320));});};
const doc=(text:string,revision="r1")=>({path:"/data/camera.json",text,revision});
beforeEach(async()=>{
 vi.resetAllMocks();experimentActive=false;pz={status:status(),ringFrames:0};
 vi.mocked(open).mockResolvedValue("/data/camera.json");
 vi.mocked(invoke).mockResolvedValue(doc('{"run":{"frame_rate_hz":1000}}'));
 vi.mocked(bridge.checkPzCameraDocument).mockResolvedValue(good);
 vi.mocked(bridge.applyPzCameraDocument).mockResolvedValue({ok:true,message:"Run setting 1000 fps, exposure auto 950 µs, applied"} as never);
 host=document.createElement("div");document.body.append(host);root=createRoot(host);
 await act(async()=>root.render(<Harness/>));
});
afterEach(async()=>{await act(async()=>root.unmount());host.remove();});
const testid=(id:string)=>host.querySelector(`[data-testid=${id}]`);
const applyButton=()=>Array.from(host.querySelectorAll("button")).find(b=>b.textContent?.startsWith("Apply Saved File"))!;

describe("the PZ7035 camera document (Config → Camera document)",()=>{
 it("uses its own editor, labels and the Service-only strobe note",()=>{
  expect(host.querySelector('section[aria-label="PZ7035 camera document editor"]')).not.toBeNull();
  expect(host.textContent).toContain("PZ7035 camera document");
  expect(host.textContent).toContain("The LED strobe itself is Service-only");
  expect(applyButton().textContent).toBe("Apply Saved File to Instrument");
 });
 it("browse loads the file from the instrument as a pz7035 document and checks it, applying nothing",async()=>{
  await act(async()=>model.run("browse"));await flush();
  expect(invoke).toHaveBeenCalledWith("camera_document",expect.objectContaining({action:"read",path:"/data/camera.json",kind:"pz7035"}));
  expect(bridge.checkPzCameraDocument).toHaveBeenCalledWith('{"run":{"frame_rate_hz":1000}}');
  expect(testid("pz-document-timing")!.textContent).toContain("1,000 fps · exposure auto 950 µs (SHS 64)");
  expect(testid("pz-document-strobe")!.textContent).toContain("exposure 50 to 1000 µs after XVS, LED strobe 7 to 67 µs");
  expect(testid("pz-document-qualification")!.textContent).toContain("5,000 fps is the qualified rate");
  expect(bridge.applyPzCameraDocument).not.toHaveBeenCalled();
 });
 it("the qualified document shows no qualification note",async()=>{
  vi.mocked(bridge.checkPzCameraDocument).mockResolvedValue({...good,qualified:true,fps:5000,exposure_us:150,vmax:256});
  await act(async()=>model.run("browse"));await flush();
  expect(testid("pz-document-qualification")).toBeNull();
 });
 it("shows the first problem and blocks apply while the document is invalid",async()=>{
  vi.mocked(bridge.checkPzCameraDocument).mockResolvedValue({ok:false,error:"run.frame_rate_hz: the Run rate must be a whole number of frames per second from 1000 to 5000"});
  await act(async()=>model.run("browse"));await flush();
  expect(testid("pz-document-error")!.textContent).toContain("from 1000 to 5000");
  expect(model.applyBlocked).toBe(true);
  await act(async()=>model.run("apply"));
  expect(bridge.applyPzCameraDocument).not.toHaveBeenCalled();
 });
 it("applies the saved, valid file through the bridge (no camera selection needed) and refreshes",async()=>{
  await act(async()=>model.run("browse"));await flush();
  expect(model.applyBlocked).toBe(false);
  await act(async()=>model.run("apply"));
  expect(bridge.applyPzCameraDocument).toHaveBeenCalledWith("/data/camera.json",false);
  expect(bridge.fetchCameraSelection).not.toHaveBeenCalled();
  expect(append).toHaveBeenCalledWith("Run setting 1000 fps, exposure auto 950 µs, applied");
  expect(refresh).toHaveBeenCalled();
 });
 it("refuses to apply an unsaved draft or a file that changed on disk",async()=>{
  await act(async()=>model.run("browse"));await flush();
  await act(async()=>model.edit('{"run":{"frame_rate_hz":2000}}'));
  expect(model.applyBlocked).toBe(true);
  await act(async()=>model.run("apply"));
  expect(bridge.applyPzCameraDocument).not.toHaveBeenCalled();
  vi.spyOn(window,"confirm").mockReturnValue(true);
  await act(async()=>model.run("reload"));await flush();
  vi.mocked(invoke).mockResolvedValue(doc('{"run":{"frame_rate_hz":3000}}',"r2"));
  await act(async()=>model.run("apply"));
  expect(bridge.applyPzCameraDocument).not.toHaveBeenCalled();
  expect(model.message).toContain("changed");
 });
 it("is blocked while an experiment runs",async()=>{
  experimentActive=true;await act(async()=>root.render(<Harness/>));
  await act(async()=>model.run("browse"));await flush();
  expect(model.applyBlocked).toBe(true);
 });
 it("asks before discarding a stopped Run's frames and sends the acknowledgement; no apply if declined",async()=>{
  pz={status:status({held:true}),ringFrames:5000};await act(async()=>root.render(<Harness/>));
  await act(async()=>model.run("browse"));await flush();
  const ask=vi.spyOn(window,"confirm").mockReturnValue(false);
  await act(async()=>model.run("apply"));
  expect(ask).toHaveBeenCalledWith(expect.stringContaining("discards the 5,000 buffered frames"));
  expect(bridge.applyPzCameraDocument).not.toHaveBeenCalled();
  ask.mockReturnValue(true);
  await act(async()=>model.run("apply"));
  expect(bridge.applyPzCameraDocument).toHaveBeenCalledWith("/data/camera.json",true);
 });
 it("a backend refusal is reported and keeps the draft",async()=>{
  vi.mocked(bridge.applyPzCameraDocument).mockResolvedValue({ok:false,message:"Stop the experiment or recording before changing the Run setting"} as never);
  await act(async()=>model.run("browse"));await flush();
  await act(async()=>model.run("apply"));
  expect(model.message).toContain("Stop the experiment");
 });
 it("bundled default is an unsaved draft of kind pz7035",async()=>{
  vi.mocked(invoke).mockResolvedValue({path:"",text:'{"run":{"frame_rate_hz":5000,"exposure_us":"auto"}}',revision:""});
  await act(async()=>model.run("default"));
  expect(invoke).toHaveBeenCalledWith("camera_document",expect.objectContaining({action:"default",kind:"pz7035"}));
  expect(model.dirty).toBe(true);expect(model.applyBlocked).toBe(true);
 });
});
