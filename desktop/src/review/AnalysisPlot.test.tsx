// @vitest-environment jsdom
import { act } from "react";
import { createRoot } from "react-dom/client";
import { expect,it } from "vitest";
import { AnalysisPlot,type ScatterArtifact } from "./AnalysisPlot";
Object.assign(globalThis,{IS_REACT_ACT_ENVIRONMENT:true});
it("renders finite positions for extreme scatter coordinates and preserves exact-key tooltips",async()=>{
 const host=document.createElement("div");const root=createRoot(host);
 const artifact:ScatterArtifact={planId:"plan",revision:"1",sourceFingerprint:"sha256:test",semantics:"display sample only",eligible:"10",spec:{kind:"scatter",x:"x",y:"y",seed:42,maxPoints:2},xUnit:"dimensionless",yUnit:"dimensionless",bounds:{x:[-Number.MAX_VALUE,Number.MAX_VALUE],y:[0,1]},points:[{population:"gate",key:"9007199254740993",x:-Number.MAX_VALUE,y:0},{population:"gate",key:"9007199254740994",x:Number.MAX_VALUE,y:1}],layers:[{population:"gate",selected:"10",unknown:"3",eligible:"10",missingCoordinates:"0"}]};
 await act(async()=>root.render(<AnalysisPlot artifact={artifact}/>));
 expect(Array.from(host.querySelectorAll("circle")).map(p=>Number(p.getAttribute("cx")))).toEqual([60,660]);
 expect(host.textContent).toContain("9007199254740993");expect(host.textContent).toContain("2 displayed of 10");
 await act(async()=>root.render(<AnalysisPlot artifact={{...artifact,points:[],bounds:null,eligible:"0"}}/>));
 expect(host.querySelectorAll("circle")).toHaveLength(0);expect(host.textContent).not.toContain("NaN");
 await act(async()=>root.unmount());
});
it("maps reverse drag from scaled screen coordinates to raw bounds and ignores cancelled or tiny gestures",async()=>{
 const host=document.createElement("div"),root=createRoot(host);const drafts:Array<unknown>=[];
 const artifact:ScatterArtifact={planId:"p",revision:"1",sourceFingerprint:"s",semantics:"sample",eligible:"0",spec:{kind:"scatter",x:"area",y:"deformability",seed:1,maxPoints:1},xUnit:"pixel2",yUnit:"dimensionless",bounds:{x:[100,200],y:[0,1]},points:[],layers:[]};
 try{
  await act(async()=>root.render(<AnalysisPlot artifact={artifact} onRectangle={draft=>drafts.push(draft)}/>));
  const svg=host.querySelector("svg")!;svg.getBoundingClientRect=()=>({left:10,top:20,width:350,height:225} as DOMRect);
  const pointer=async(type:string,x:number,y:number)=>act(async()=>{const event=new Event(type,{bubbles:true});Object.assign(event,{pointerId:1,button:0,clientX:10+x/2,clientY:20+y/2});svg.dispatchEvent(event);});
  await pointer("pointerdown",510,110);await pointer("pointermove",210,290);await pointer("pointerup",210,290);
  expect(drafts).toEqual([{x:"area",y:"deformability",bounds:[125,175,.25,.75]}]);
  await pointer("pointerdown",100,100);await pointer("pointercancel",300,300);await pointer("pointerup",300,300);
  await pointer("pointerdown",100,100);await pointer("pointerup",101,101);expect(drafts).toHaveLength(1);
 }finally{await act(async()=>root.unmount());}
});
