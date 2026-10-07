// @vitest-environment jsdom
import {act} from "react";
import {createRoot,Root} from "react-dom/client";
import {afterEach,beforeEach,expect,it,vi} from "vitest";
import {invoke} from "@tauri-apps/api/core";
import {open} from "@tauri-apps/plugin-dialog";
import {ProfilesPanel,useProfiles} from "./profiles";
vi.mock("@tauri-apps/api/core",()=>({invoke:vi.fn()}));
vi.mock("@tauri-apps/plugin-dialog",()=>({open:vi.fn()}));
Object.assign(globalThis,{IS_REACT_ACT_ENVIRONMENT:true});
const storage=new Map<string,string>();
Object.defineProperty(window,"localStorage",{configurable:true,value:{getItem:(k:string)=>storage.get(k)??null,setItem:(k:string,v:string)=>storage.set(k,v)}});
let host:HTMLDivElement,root:Root,model:ReturnType<typeof useProfiles>,visible=true;
const profile={name:"original",path:"/profiles/original/config.json",revision:"rev1",document_json:'{"unknown":17}',script:"// original"};
const ctx={currentConfig:null as string|null,ready:true,active:false,resume:false,append:vi.fn(),onOpen:vi.fn().mockResolvedValue(undefined)};
function App(){model=useProfiles(ctx);return visible?<ProfilesPanel model={model}/>:null;}
beforeEach(async()=>{vi.clearAllMocks();visible=true;ctx.currentConfig=null;ctx.active=false;ctx.resume=false;window.localStorage.setItem("mib.profiles.directory","/profiles");vi.spyOn(window,"confirm").mockReturnValue(true);vi.mocked(invoke).mockImplementation(async(_c,args)=>JSON.parse((args as {request:string}).request).operation==="list"?{ok:true,profiles:[profile]}:{ok:true,profile});host=document.createElement("div");document.body.append(host);root=createRoot(host);await act(async()=>root.render(<App/>));await act(async()=>model.run("read",profile));});
afterEach(async()=>{await act(async()=>root.unmount());host.remove();vi.restoreAllMocks();});
it("keeps complete config and optional script drafts across navigation",async()=>{await act(async()=>model.edit('{"unknown":18}'));visible=false;await act(async()=>root.render(<App/>));visible=true;await act(async()=>root.render(<App/>));expect(model.document).toContain("18");expect(model.script).toBe("// original");expect(model.dirty).toBe(true);});
it("uses authoritative baseline for rename and preserves state on conflict",async()=>{vi.mocked(invoke).mockResolvedValue({ok:false,error:"Profile changed; reload"});await act(async()=>model.setName("renamed"));await act(async()=>model.run("rename"));expect(model.selected?.revision).toBe("rev1");expect(model.message).toContain("reload");expect(JSON.parse((vi.mocked(invoke).mock.lastCall![1] as {request:string}).request)).toMatchObject({name:"original",destination:"renamed",baseline:"rev1"});});
it("rejects invalid draft locally without mutation",async()=>{await act(async()=>model.edit("[]"));vi.mocked(invoke).mockClear();await act(async()=>model.run("create"));expect(invoke).not.toHaveBeenCalled();expect(model.dirty).toBe(true);});
it("does not discard unsaved draft when profile switch is cancelled",async()=>{await act(async()=>model.edit('{"keep":true}'));vi.mocked(window.confirm).mockReturnValue(false);vi.mocked(invoke).mockClear();await act(async()=>model.run("read",profile));expect(invoke).not.toHaveBeenCalled();expect(model.document).toContain("keep");});
it("serializes double submissions and rejects commands during an experiment",async()=>{let resolve!:(r:unknown)=>void;vi.mocked(invoke).mockImplementationOnce(()=>new Promise(r=>{resolve=r;}));let operation!:Promise<void>;await act(async()=>{operation=model.run("create");void model.run("create");});expect(model.busy).toBe(true);await act(async()=>{resolve({ok:false,error:"collision"});await operation;});ctx.active=true;await act(async()=>root.render(<App/>));vi.mocked(invoke).mockClear();await act(async()=>model.run("create"));expect(invoke).not.toHaveBeenCalled();});

it("reload of an idle native session reads identity without replaying startup selection",async()=>{
 await act(async()=>root.unmount());ctx.resume=true;vi.mocked(invoke).mockClear();root=createRoot(host);
 await act(async()=>root.render(<App/>));
 const operations=vi.mocked(invoke).mock.calls.map(([,args])=>JSON.parse((args as {request:string}).request).operation);
 expect(operations).not.toContain("restore");
 expect(operations).toContain("selection");
});

it("seeds a new draft from the currently loaded complete config document",async()=>{
 ctx.currentConfig=' {"image_processing":{"threshold":42},"unknown":{"keep":true}} ';
 await act(async()=>root.render(<App/>));
 vi.mocked(invoke).mockClear();
 await act(async()=>model.run("new"));
 expect(invoke).not.toHaveBeenCalled();
 expect(model.document).toBe(ctx.currentConfig);
 expect(model.selected).toBeNull();
 expect(model.script).toBeNull();
 expect(model.dirty).toBe(true);
 expect(model.name).toBe("");
});
it("disables new draft without an open config document and guards direct calls",async()=>{
 const button=Array.from(host.querySelectorAll("button")).find(b=>b.textContent==="New Draft from Current Config")!;
 expect(button.disabled).toBe(true);
 expect(button.title).toContain("App config editor");
 expect(button.title).toContain("Import Config to Draft…");
 const previous=model.document;
 vi.mocked(invoke).mockClear();
 await act(async()=>model.run("new"));
 expect(model.document).toBe(previous);
 expect(invoke).not.toHaveBeenCalled();
 expect(model.message).toContain("App config editor");
});
it("choosing a folder preserves the draft without a discard prompt or config fetch",async()=>{
 await act(async()=>{model.edit('{"keep":true}');model.setName("draft-name");});
 vi.mocked(window.confirm).mockClear();
 vi.mocked(open).mockResolvedValueOnce("/other-profiles");
 vi.mocked(invoke).mockResolvedValue({ok:true,profiles:[]});
 await act(async()=>model.run("choose"));
 expect(window.confirm).not.toHaveBeenCalled();
 expect(model.base).toBe("/other-profiles");
 expect(model.document).toBe('{"keep":true}');
 expect(model.name).toBe("draft-name");
 expect(model.script).toBe("// original");
 expect(model.dirty).toBe(true);
 expect(vi.mocked(invoke).mock.calls.every(([command])=>command==="profile_command")).toBe(true);
});
it.each(["", "{}", " { \n } "])("refuses empty draft %j with a reason",async draft=>{
 await act(async()=>{model.edit(draft);model.setName("new-profile");});
 const button=Array.from(host.querySelectorAll("button")).find(b=>b.textContent==="Save Draft as New Profile")!;
 expect(button.disabled).toBe(true);
 expect(button.title).toContain("empty");
 vi.mocked(invoke).mockClear();
 await act(async()=>model.run("create"));
 expect(invoke).not.toHaveBeenCalled();
 expect(model.message).toContain("empty");
});
it("saves a populated draft as a new profile",async()=>{
 await act(async()=>{model.edit('{"image_processing":{"threshold":42}}');model.setName("new-profile");});
 vi.mocked(invoke).mockClear();
 await act(async()=>model.run("create"));
 expect(JSON.parse((vi.mocked(invoke).mock.calls[0][1] as {request:string}).request)).toMatchObject({operation:"create",name:"new-profile",document_json:'{"image_processing":{"threshold":42}}'});
});

it.each(["[]", "null", "42", '\"text\"', "{invalid"])("disables and refuses invalid draft %j",async draft=>{
 await act(async()=>{model.edit(draft);model.setName("new-profile");});
 const button=Array.from(host.querySelectorAll("button")).find(b=>b.textContent==="Save Draft as New Profile")!;
 expect(button.disabled).toBe(true);
 expect(button.title).toMatch(/object|valid JSON/);
 vi.mocked(invoke).mockClear();
 await act(async()=>model.run("create"));
 expect(invoke).not.toHaveBeenCalled();
 expect(model.message).toMatch(/object|valid JSON/);
});
