// @vitest-environment jsdom
import {expect,it} from "vitest";
import {renderToStaticMarkup} from "react-dom/server";
import {PopulationGraphEditor,renamePopulation,removePopulation} from "./PopulationGraphEditor";
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
