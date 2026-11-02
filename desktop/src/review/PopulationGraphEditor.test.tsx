// @vitest-environment jsdom
import {act} from "react";
import {createRoot} from "react-dom/client";
// @vitest-environment jsdom
import {expect,it} from "vitest";
import {renderToStaticMarkup} from "react-dom/server";
import {PopulationGraphEditor,addRectangle,renamePopulation,removePopulation} from "./PopulationGraphEditor";
import type {PopulationGraph} from "./PopulationAnalysis";
const graph:PopulationGraph={id:"p",revision:"draft",output:"both",nodes:[{id:"a",parent:"",kind:"interval",feature:"area",minimum:null,maximum:null},{id:"aa",parent:"a",kind:"interval",feature:"area",minimum:0,maximum:1},{id:"both",parent:"",kind:"and",operands:["a","aa"]}]};
it("renames exact dependencies and output without substring rewriting",()=>{
 const next=renamePopulation(graph,"a","renamed");
 expect(next.nodes[1].id).toBe("aa");expect(next.nodes[1].parent).toBe("renamed");
 expect(next.nodes[2]).toMatchObject({operands:["renamed","aa"]});
 expect(renamePopulation(graph,"both","out").output).toBe("out");
 expect(()=>renamePopulation(graph,"a","aa")).toThrow();expect(()=>removePopulation(graph,"a")).toThrow();expect(()=>removePopulation(graph,"both")).toThrow();
});
it("malformed advanced JSON fails closed without crashing the form",()=>{
 const bad={...graph,nodes:[{id:"bad",parent:"",kind:"and"}]};
 expect(renderToStaticMarkup(<PopulationGraphEditor text={JSON.stringify(bad)} onChange={()=>{}} disabled={false}/>)).toContain("no draft was discarded");
});

it("creates a transparent native rectangle composition and rejects collisions or reversed bounds",()=>{
 const next=addRectangle(graph,"rect","area","deformability",[0,10,null,1]);
 expect(next.output).toBe("rect");expect(next.nodes.slice(-3)).toEqual([
 {id:"rect-x",parent:"",kind:"interval",feature:"area",minimum:0,maximum:10},
 {id:"rect-y",parent:"",kind:"interval",feature:"deformability",minimum:null,maximum:1},
 {id:"rect",parent:"",kind:"and",operands:["rect-x","rect-y"]}]);
 expect(()=>addRectangle(next,"rect","area","deformability",[0,1,0,1])).toThrow();
 expect(()=>addRectangle(graph,"rect","area","deformability",[10,0,0,1])).toThrow();
});
it("edits ellipse nodes without treating geometry as Boolean operands",()=>{
 const ellipse:PopulationGraph={id:"p",revision:"1",output:"e",nodes:[{id:"e",parent:"",kind:"ellipse",x:"area",y:"deformability",centerX:100,centerY:0.1,radiusX:5,radiusY:0.05}]};
 expect(renamePopulation(ellipse,"e","renamed").output).toBe("renamed");
 expect(renderToStaticMarkup(<PopulationGraphEditor text={JSON.stringify(ellipse)} onChange={()=>{}} disabled={false}/>)).toContain("Ellipse radiusX");
});
it("renders quadrant boundary ownership and preserves typed region on rename",()=>{
 const quadrant:PopulationGraph={id:"p",revision:"1",output:"q",nodes:[{id:"q",parent:"",kind:"quadrant",x:"area",y:"deformability",thresholdX:100,thresholdY:0.1,quadrant:"upper-right"}]};
 expect(renamePopulation(quadrant,"q","renamed").nodes[0]).toMatchObject({id:"renamed",quadrant:"upper-right"});
 expect(renderToStaticMarkup(<PopulationGraphEditor text={JSON.stringify(quadrant)} onChange={()=>{}} disabled={false}/>)).toContain("threshold belong right");
});

it("authors polygon vertices without Boolean coercion and rejects malformed documents",()=>{
 const polygon:PopulationGraph={id:"p",revision:"1",output:"g",nodes:[{id:"g",parent:"",kind:"polygon",x:"area",y:"deformability",vertices:[[0,0],[2,0],[0,2]]}]};
 expect(renamePopulation(polygon,"g","renamed").nodes[0]).toMatchObject({kind:"polygon",vertices:[[0,0],[2,0],[0,2]]});
 expect(renderToStaticMarkup(<PopulationGraphEditor text={JSON.stringify(polygon)} onChange={()=>{}} disabled={false}/>)).toContain("Polygon vertices");
 expect(renderToStaticMarkup(<PopulationGraphEditor text={JSON.stringify({...polygon,nodes:[{...polygon.nodes[0],vertices:[null]}]})} onChange={()=>{}} disabled={false}/>)).toContain("no draft was discarded");
});

it("converts a gate to polygon and applies vertex edits through real controls",async()=>{
 Object.assign(globalThis,{IS_REACT_ACT_ENVIRONMENT:true});
 const host=document.createElement("div"),root=createRoot(host);let text=JSON.stringify(graph);
 const render=()=>root.render(<PopulationGraphEditor text={text} onChange={next=>{text=next;render();}} disabled={false}/>);
 try {
  await act(async()=>render());
  const select=Array.from(host.querySelectorAll("select")).find(s=>s.parentElement?.textContent?.startsWith("Gate type"))!;
  await act(async()=>{select.value="polygon";select.dispatchEvent(new Event("change",{bubbles:true}));});
  expect(JSON.parse(text).nodes[0].kind).toBe("polygon");
  const textarea=host.querySelector("textarea")!;
  await act(async()=>{textarea.value="[[1,2],[3,2],[1,4]]";textarea.dispatchEvent(new FocusEvent("focusout",{bubbles:true}));});
  expect(JSON.parse(text).nodes[0].vertices).toEqual([[1,2],[3,2],[1,4]]);
  const before=text;
  await act(async()=>{const field=host.querySelector("textarea")!;field.value="[[0,null]]";field.dispatchEvent(new FocusEvent("focusout",{bubbles:true}));});
  expect(text).toBe(before);expect(host.querySelector('[role="alert"]')).not.toBeNull();
 }finally{await act(async()=>root.unmount());}
});
