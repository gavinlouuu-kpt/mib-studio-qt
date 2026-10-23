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
