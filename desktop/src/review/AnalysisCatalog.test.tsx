// @vitest-environment jsdom
import {act} from "react";
import {createRoot} from "react-dom/client";
import {expect,it,vi} from "vitest";
import {AnalysisCatalog} from "./AnalysisCatalog";
Object.assign(globalThis,{IS_REACT_ACT_ENVIRONMENT:true});
it("pages with the backend cursor and opens the displayed exact revision",async()=>{
 const host=document.createElement("div"),root=createRoot(host),read=vi.fn(),open=vi.fn().mockResolvedValue(undefined);
 read.mockResolvedValueOnce({status:"ok",entries:[{id:"one",revision:"3"}],next:"one"}).mockResolvedValueOnce({status:"ok",entries:[{id:"two",revision:"7"}],next:null});
 try{
  await act(async()=>root.render(<AnalysisCatalog read={read} disabled={false} onOpen={open}/>));
  const click=async(text:string)=>act(async()=>Array.from(host.querySelectorAll("button")).find(b=>b.textContent===text)!.click());
  await click("Discover saved library records");await click("Open one revision 3");expect(open).toHaveBeenCalledWith("one","3");
  await click("Next discovery page");expect(read.mock.calls[1][0]).toEqual({action:"catalog",kind:"library",after:"one",limit:25});expect(host.textContent).toContain("two");expect(host.textContent).not.toContain("Open one revision 3");
 }finally{await act(async()=>root.unmount());}
});
