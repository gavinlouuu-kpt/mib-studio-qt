// @vitest-environment jsdom
import {act} from "react";
import {createRoot,Root} from "react-dom/client";
import {beforeEach,afterEach,it,expect,vi} from "vitest";
import {invoke} from "@tauri-apps/api/core";
import {open} from "@tauri-apps/plugin-dialog";
import {bridge,type CameraSelection} from "./bridge";
import {useCameraDocument} from "./cameraDocument";
vi.mock("@tauri-apps/api/core",()=>({invoke:vi.fn()}));vi.mock("@tauri-apps/plugin-dialog",()=>({open:vi.fn()}));
vi.mock("./bridge",()=>({bridge:{fetchCameraSelection:vi.fn(),applyCameraScript:vi.fn(),selectMindVisionCamera:vi.fn()}}));
Object.assign(globalThis,{IS_REACT_ACT_ENVIRONMENT:true});
let root:Root,model:ReturnType<typeof useCameraDocument>,kind:"js"|"json",running:boolean;
const selected={valid:true,configured:true,running:false,mode:3,mindvision_index:2,interface_index:-1,device_index:-1,label:"MV"} as CameraSelection;
function Harness(){model=useCameraDocument({ready:true,running,experimentActive:false,selection:selected,append:vi.fn(),refresh:vi.fn()},kind);return null;}
beforeEach(async()=>{vi.resetAllMocks();kind="json";running=false;vi.mocked(open).mockResolvedValue("/tmp/camera.json");vi.mocked(invoke).mockResolvedValue({path:"/tmp/camera.json",text:"{}",revision:"r1"});vi.mocked(bridge.fetchCameraSelection).mockResolvedValue(selected);vi.mocked(bridge.selectMindVisionCamera).mockResolvedValue({ok:true,message:"Applied"} as never);root=createRoot(document.createElement("div"));await act(async()=>root.render(<Harness/>));});
afterEach(async()=>{await act(async()=>root.unmount());});
it("mount/browse/save do not actuate cameras; checked revision accompanies save",async()=>{expect(invoke).not.toHaveBeenCalled();await act(async()=>model.run("browse"));await act(async()=>model.edit('{"exposure":50}'));await act(async()=>model.run("save"));expect(invoke).toHaveBeenLastCalledWith("camera_document",expect.objectContaining({action:"save",baseline:"r1",text:'{"exposure":50}'}));expect(bridge.selectMindVisionCamera).not.toHaveBeenCalled();});
it("applies the saved file to the same stopped MindVision selection",async()=>{await act(async()=>model.run("browse"));await act(async()=>model.run("apply"));expect(bridge.selectMindVisionCamera).toHaveBeenCalledWith(2,"MV","/tmp/camera.json");});
it("rejects dirty, running, changed-camera and changed-file apply",async()=>{await act(async()=>model.run("browse"));await act(async()=>model.edit("changed"));await act(async()=>model.run("apply"));expect(bridge.selectMindVisionCamera).not.toHaveBeenCalled();vi.spyOn(window,"confirm").mockReturnValue(true);await act(async()=>model.run("reload"));running=true;await act(async()=>root.render(<Harness/>));await act(async()=>model.run("apply"));expect(bridge.selectMindVisionCamera).not.toHaveBeenCalled();running=false;await act(async()=>root.render(<Harness/>));vi.mocked(bridge.fetchCameraSelection).mockResolvedValue({...selected,mindvision_index:3});await act(async()=>model.run("apply"));expect(bridge.selectMindVisionCamera).not.toHaveBeenCalled();vi.mocked(bridge.fetchCameraSelection).mockResolvedValue(selected);vi.mocked(invoke).mockResolvedValue({path:"/tmp/camera.json",text:"{}",revision:"r2"});await act(async()=>model.run("apply"));expect(bridge.selectMindVisionCamera).not.toHaveBeenCalled();});
it("failed save preserves the edited draft and baseline",async()=>{await act(async()=>model.run("browse"));await act(async()=>model.edit("draft"));vi.mocked(invoke).mockRejectedValue(new Error("changed on disk"));await act(async()=>model.run("save"));expect(model.dirty).toBe(true);expect(model.text).toBe("draft");expect(model.doc?.revision).toBe("r1");});
