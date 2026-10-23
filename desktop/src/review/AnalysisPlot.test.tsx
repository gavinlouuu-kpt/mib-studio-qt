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
