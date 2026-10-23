// @vitest-environment jsdom
import {act} from "react";
import {createRoot,type Root} from "react-dom/client";
import {beforeEach,afterEach,it,expect,vi} from "vitest";
import {RecipeScience} from "./RecipeScience";
import type {Recipe} from "./recipeModel";
vi.mock("../transport",()=>({invoke:vi.fn()}));
import {invoke} from "../transport";
const call=vi.mocked(invoke);
const recipe:Recipe={schema_version:1,id:"recipe",revision:"1",title:"R",question:"",method:{source:"local",id:"method",revision:"sha"},analysis_plan:{id:"plan",revision:"3"},lineage:{recipe:null,finding:null}};
let root:Root,host:HTMLDivElement;
const save=vi.fn(),open=vi.fn(async()=>{});
const render=(revision="2")=>act(()=>root.render(<RecipeScience workspaceId="workspace" revision={revision} recipe={recipe} sources={{run:{path:"/run.h5",sha256:"abc",size_bytes:"10"}}} controlHeld blocked={false} profileBase="/profiles" onOpenSource={open} onSaveFinding={save}/>));
const button=(text:string)=>[...host.querySelectorAll("button")].find(b=>b.textContent===text)!;
const click=async(text:string)=>act(async()=>button(text).click());
const select=()=>act(()=>{const s=host.querySelector("select")!;s.value="run";s.dispatchEvent(new Event("change",{bubbles:true}));});
beforeEach(()=>{Object.assign(globalThis,{IS_REACT_ACT_ENVIRONMENT:true});call.mockReset();save.mockReset();open.mockClear();host=document.createElement("div");root=createRoot(host);});
afterEach(()=>act(()=>root.unmount()));
const evaluation={schema:"F-evaluation/1",planId:"plan",revision:"3",selected:"10",excluded:"4",unknown:"0",measured:"10",missing:"0",mean:104.6,median:104.5,minimum:100,maximum:110,denominator:"14",unit:"px²",feature:"area",populationComplete:true,sourceFingerprint:"sha256:abc"};
it("evaluates only explicitly through native pinned workspace request and saves exact evidence separately from interpretation",async()=>{
 render();expect(call).not.toHaveBeenCalled();select();
 await click("Open linked source in Review");expect(open).toHaveBeenCalledWith("/run.h5");expect(call).not.toHaveBeenCalled();
 call.mockResolvedValue({status:"ok",workspace_id:"workspace",revision:"2",source_id:"run",evaluation});
 await click("Evaluate pinned recipe");expect(JSON.parse((call.mock.calls[0][1] as {request:string}).request)).toMatchObject({action:"evaluate_recipe",workspace_id:"workspace",revision:"2",source_id:"run",source_kind:"stored"});
 expect(host.textContent).toContain("104.6");await click("Save finding in workspace");expect(save.mock.calls[0][0]).toMatchObject({recipe,source_id:"run",source_sha256:"abc",evaluation});
});
it("rejects mismatched analysis revisions and drops late results after workspace changes",async()=>{
 render();select();call.mockResolvedValueOnce({status:"ok",workspace_id:"workspace",revision:"2",source_id:"run",evaluation:{...evaluation,revision:"4"}});
 await click("Evaluate pinned recipe");expect(host.textContent).toContain("Mismatched pinned analysis revision");expect(button("Save finding in workspace")).toBeUndefined();
 let reply!:(x:unknown)=>void;call.mockImplementationOnce(()=>new Promise(resolve=>{reply=resolve;}) as ReturnType<typeof invoke>);
 await click("Evaluate pinned recipe");render("3");
 await act(async()=>reply({status:"ok",workspace_id:"workspace",revision:"2",source_id:"run",evaluation}));
 expect(button("Save finding in workspace")).toBeUndefined();expect(host.textContent).not.toContain("104.6");
});
