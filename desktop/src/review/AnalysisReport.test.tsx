// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { beforeEach, afterEach, expect, it, vi } from "vitest";
import { AnalysisReport } from "./AnalysisReport";
Object.assign(globalThis,{IS_REACT_ACT_ENVIRONMENT:true});
let host:HTMLDivElement, root:Root;
const read=vi.fn(),write=vi.fn();const key="mib.analysis.pending-report.v1";
const command={action:"reportBuild",reportId:"report",revision:"1",runId:"run",commandId:"same-command"};
beforeEach(async()=>{
 vi.resetAllMocks();
 const storage=new Map<string,string>();
 Object.defineProperty(window,"localStorage",{configurable:true,value:{getItem:(k:string)=>storage.get(k)??null,setItem:(k:string,v:string)=>storage.set(k,v),removeItem:(k:string)=>storage.delete(k)}});
 host=document.createElement("div");document.body.append(host);root=createRoot(host);
 await act(async()=>root.render(<AnalysisReport read={read} write={write} disabled={false}/>));
});
afterEach(async()=>{await act(async()=>root.unmount());host.remove();});
async function reconcile(){await act(async()=>Array.from(host.querySelectorAll("button")).find(b=>b.textContent==="Reconcile pending report")!.click());}
it("retries an immutable report with the original command after lost reply",async()=>{
 window.localStorage.setItem(key,JSON.stringify(command));write.mockRejectedValueOnce(new Error("lost reply"));
 await reconcile();expect(JSON.parse(window.localStorage.getItem(key)!)).toEqual(command);
 write.mockResolvedValue({status:"ok",document:{request:command,contentHash:"hash",artifact:{title:"Pinned",reportId:"report",revision:"1",blocks:[]}}});
 await reconcile();expect(write.mock.calls[1][0]).toEqual(command);expect(read).not.toHaveBeenCalled();expect(window.localStorage.getItem(key)).toBeNull();
 expect(host.textContent).toContain("Pinned");
});
it("renders report titles as text, never executable markup",async()=>{
 window.localStorage.setItem(key,JSON.stringify(command));write.mockResolvedValue({status:"ok",document:{request:command,contentHash:"hash",artifact:{title:'<img src=x onerror="alert(1)">',reportId:"report",revision:"1",blocks:[]}}});
 await reconcile();expect(host.querySelector("article img")).toBeNull();expect(host.textContent).toContain("<img src=x");
});
it("renders persisted native table columns in order without inventing omitted statistics",async()=>{
 window.localStorage.setItem(key,JSON.stringify(command));
 write.mockResolvedValue({status:"ok",document:{request:command,contentHash:"hash",artifact:{title:"Table",reportId:"report",revision:"1",blocks:[{spec:{id:"t",kind:"table",title:"Selected columns",reference:{planId:"p",revision:"1"}},result:{report:{columns:["population","feature","unit","median"],table:[{population:"g",feature:"area",unit:"pixel2",median:5}]}}}]}}});
 await reconcile();expect(Array.from(host.querySelectorAll("th")).map(e=>e.textContent)).toEqual(["population","feature","unit","median"]);
 expect(Array.from(host.querySelectorAll("td")).map(e=>e.textContent)).toEqual(["g","area","pixel2","5"]);
});
