import { useEffect,useRef,useState } from "react";
import type { PopulationGraph } from "./PopulationAnalysis";
type Node=PopulationGraph["nodes"][number];
export function renamePopulation(graph:PopulationGraph,id:string,next:string):PopulationGraph{
 if(!next.trim()||graph.nodes.some(n=>n.id===next&&n.id!==id))throw new Error("Population ID must be nonempty and unique.");
 return {...graph,output:graph.output===id?next:graph.output,nodes:graph.nodes.map(n=>({...n,id:n.id===id?next:n.id,parent:n.parent===id?next:n.parent,...((n.kind==="and"||n.kind==="or"||n.kind==="not")?{operands:n.operands.map(o=>o===id?next:o)}:{})}))};
}
export function removePopulation(graph:PopulationGraph,id:string):PopulationGraph{
 if(graph.output===id||graph.nodes.some(n=>n.parent===id||((n.kind==="and"||n.kind==="or"||n.kind==="not")&&n.operands.includes(id))))throw new Error("Select another output and remove dependent references before deleting this population.");
 return {...graph,nodes:graph.nodes.filter(n=>n.id!==id)};
}
/** A rectangle is an explicit pair of inclusive native interval gates and AND. */
export function addRectangle(graph:PopulationGraph,id:string,x:string,y:string,bounds:(number|null)[]):PopulationGraph{
 const ids=[`${id}-x`,`${id}-y`,id];
 if(!id.trim()||!x.trim()||!y.trim()||x===y||graph.nodes.length>253||ids.some(key=>graph.nodes.some(n=>n.id===key)))throw new Error("Rectangle needs a unique ID, two distinct parameters and room for three populations.");
 if(bounds.length!==4||bounds.some(v=>v!==null&&!Number.isFinite(v))||(bounds[0]!==null&&bounds[1]!==null&&bounds[0]>bounds[1])||(bounds[2]!==null&&bounds[3]!==null&&bounds[2]>bounds[3]))throw new Error("Rectangle bounds must be finite, ordered or blank.");
 return {...graph,output:id,nodes:[...graph.nodes,{id:ids[0],parent:"",kind:"interval",feature:x,minimum:bounds[0],maximum:bounds[1]},{id:ids[1],parent:"",kind:"interval",feature:y,minimum:bounds[2],maximum:bounds[3]},{id,parent:"",kind:"and",operands:ids.slice(0,2)}]};
}
/** Draft structure editing only. Native code remains the graph and numerical authority. */
export function PopulationGraphEditor({text,onChange,disabled}:{text:string;onChange:(text:string)=>void;disabled:boolean}){
 const [rectangleX,setRectangleX]=useState("area"),[rectangleY,setRectangleY]=useState("deformability");
 const [rectangleBounds,setRectangleBounds]=useState(["","","",""]);
 const [newId,setNewId]=useState("");const [error,setError]=useState("");const [,redraw]=useState(0);
 const undo=useRef<string[]>([]),redo=useRef<string[]>([]),emitted=useRef(text);
 useEffect(()=>{if(text!==emitted.current){undo.current=[];redo.current=[];emitted.current=text;redraw(n=>n+1);}},[text]);
 let graph:PopulationGraph;
 try{graph=JSON.parse(text);if(typeof graph.id!=="string"||!Array.isArray(graph.nodes)||graph.nodes.some(n=>!n||typeof n.id!=="string"||typeof n.parent!=="string"||(n.kind==="interval"?typeof n.feature!=="string":n.kind==="ellipse"?(typeof n.x!=="string"||typeof n.y!=="string"||![n.centerX,n.centerY,n.radiusX,n.radiusY].every(v=>typeof v==="number"&&Number.isFinite(v))):n.kind==="quadrant"?(typeof n.x!=="string"||typeof n.y!=="string"||!Number.isFinite(n.thresholdX)||!Number.isFinite(n.thresholdY)||!["lower-left","lower-right","upper-left","upper-right"].includes(n.quadrant)):(!Array.isArray(n.operands)||n.operands.some(o=>typeof o!=="string")))||!["interval","ellipse","quadrant","and","or","not"].includes(n.kind)))throw new Error();}
 catch{return <p>Gate editor needs a valid graph document. Correct the advanced JSON below; no draft was discarded.</p>;}
 const publish=(value:string)=>{emitted.current=value;onChange(value);redraw(n=>n+1);};
 const edit=(operation:(g:PopulationGraph)=>PopulationGraph)=>{try{const next=JSON.stringify(operation(graph),null,2);if(next!==text){undo.current=[...undo.current.slice(-19),text];redo.current=[];publish(next);}setError("");}catch(e){setError(String(e));}};
 const replace=(id:string,node:Node)=>edit(g=>({...g,nodes:g.nodes.map(n=>n.id===id?node:n)}));
 const bound=(value:string)=>{if(!value.trim())return null;const n=Number(value);if(!Number.isFinite(n))throw new Error("Bound must be finite or blank.");return n;};
 return <section aria-label="Population graph editor"><fieldset disabled={disabled}>
  <label>Plan ID<input aria-label="Graph plan ID" key={graph.id} defaultValue={graph.id} onBlur={e=>edit(g=>({...g,id:e.target.value}))}/></label>
  <label>Output population<select aria-label="Graph output" value={graph.output} onChange={e=>edit(g=>({...g,output:e.target.value}))}>{graph.nodes.map(n=><option key={n.id} value={n.id}>{n.id}</option>)}</select></label>
  <label>New population ID<input value={newId} onChange={e=>setNewId(e.target.value)}/></label>
  <button disabled={!newId.trim()||graph.nodes.length>=256} onClick={()=>edit(g=>{if(g.nodes.some(n=>n.id===newId))throw new Error("Population ID already exists.");return {...g,nodes:[...g.nodes,{id:newId,parent:"",kind:"interval",feature:"area",minimum:null,maximum:null}]};})}>Add population</button>
  <details><summary>Add rectangular population</summary>
   <p>Uses the new population ID above. Creates two inclusive native interval populations and their AND; blank bounds are unbounded. These named components remain editable and exportable.</p>
   <label>Rectangle X parameter<input value={rectangleX} onChange={e=>setRectangleX(e.target.value)}/></label>
   <label>Rectangle Y parameter<input value={rectangleY} onChange={e=>setRectangleY(e.target.value)}/></label>
   {["X minimum","X maximum","Y minimum","Y maximum"].map((label,index)=><label key={label}>{label}<input value={rectangleBounds[index]} onChange={e=>setRectangleBounds(values=>values.map((v,i)=>i===index?e.target.value:v))}/></label>)}
   <button disabled={!newId.trim()||graph.nodes.length>253} onClick={()=>edit(g=>addRectangle(g,newId,rectangleX,rectangleY,rectangleBounds.map(bound)))}>Add inclusive rectangle</button>
  </details>
  {graph.nodes.map(node=><fieldset key={node.id}><legend>{node.id}</legend>
   <label>Population ID<input aria-label={`Rename ${node.id}`} defaultValue={node.id} onBlur={e=>edit(g=>renamePopulation(g,node.id,e.target.value))}/></label>
   <label>Gate type<select value={node.kind} onChange={e=>{const kind=e.target.value as Node["kind"];const next:Node=kind==="interval"?{id:node.id,parent:node.parent,kind,feature:"area",minimum:null,maximum:null}:kind==="ellipse"?{id:node.id,parent:node.parent,kind,x:"area",y:"deformability",centerX:100,centerY:0.1,radiusX:10,radiusY:0.1}:kind==="quadrant"?{id:node.id,parent:node.parent,kind,x:"area",y:"deformability",thresholdX:100,thresholdY:0.1,quadrant:"upper-right"}:{id:node.id,parent:node.parent,kind,operands:graph.nodes.filter(n=>n.id!==node.id).slice(0,kind==="not"?1:2).map(n=>n.id)};replace(node.id,next);}}><option value="interval">Inclusive interval</option><option value="ellipse">Axis-aligned ellipse</option><option value="quadrant">Quadrant</option><option value="and">AND</option><option value="or">OR</option><option value="not">NOT</option></select></label>
   <label>Parent population<select value={node.parent} onChange={e=>replace(node.id,{...node,parent:e.target.value})}><option value="">All observations</option>{graph.nodes.filter(n=>n.id!==node.id).map(n=><option key={n.id} value={n.id}>{n.id}</option>)}</select></label>
   {node.kind==="interval"?<>
    <label>Parameter<input key={node.feature} defaultValue={node.feature} onBlur={e=>replace(node.id,{...node,feature:e.target.value})}/></label>
    <label>Minimum (blank unbounded)<input key={`min-${node.minimum}`} defaultValue={node.minimum??""} onBlur={e=>edit(g=>({...g,nodes:g.nodes.map(n=>n.id===node.id?{...node,minimum:bound(e.target.value)}:n)}))}/></label>
    <label>Maximum (blank unbounded)<input key={`max-${node.maximum}`} defaultValue={node.maximum??""} onBlur={e=>edit(g=>({...g,nodes:g.nodes.map(n=>n.id===node.id?{...node,maximum:bound(e.target.value)}:n)}))}/></label>
   </>:node.kind==="ellipse"?<>
    {(["x","y"] as const).map(key=><label key={key}>Ellipse {key} parameter<input key={node[key]} defaultValue={node[key]} onBlur={e=>replace(node.id,{...node,[key]:e.target.value})}/></label>)}
    {(["centerX","centerY","radiusX","radiusY"] as const).map(key=><label key={key}>Ellipse {key}<input key={node[key]} type="number" step="any" defaultValue={node[key]} onBlur={e=>edit(g=>{const value=bound(e.target.value);if(value===null||(key.startsWith("radius")&&value<=0))throw new Error("Ellipse geometry must be finite; radii must be positive.");return {...g,nodes:g.nodes.map(n=>n.id===node.id?{...node,[key]:value}:n)};})}/></label>)}
   </>:node.kind==="quadrant"?<>
    {(["x","y"] as const).map(key=><label key={key}>Quadrant {key} parameter<input key={node[key]} defaultValue={node[key]} onBlur={e=>replace(node.id,{...node,[key]:e.target.value})}/></label>)}
    {(["thresholdX","thresholdY"] as const).map(key=><label key={key}>Quadrant {key}<input key={node[key]} type="number" step="any" defaultValue={node[key]} onBlur={e=>edit(g=>{const value=bound(e.target.value);if(value===null)throw new Error("Quadrant thresholds must be finite.");return {...g,nodes:g.nodes.map(n=>n.id===node.id?{...node,[key]:value}:n)};})}/></label>)}
    <label>Quadrant region<select value={node.quadrant} onChange={e=>replace(node.id,{...node,quadrant:e.target.value as typeof node.quadrant})}>{["lower-left","lower-right","upper-left","upper-right"].map(q=><option key={q}>{q}</option>)}</select></label>
    <p>Values on the X threshold belong right; values on the Y threshold belong upper. Missing coordinates remain unknown.</p>
   </>:<div>Operands {graph.nodes.filter(n=>n.id!==node.id).map(n=><label key={n.id}><input type="checkbox" checked={node.operands.includes(n.id)} onChange={e=>replace(node.id,{...node,operands:node.kind==="not"?(e.target.checked?[n.id]:[]):e.target.checked?[...node.operands,n.id]:node.operands.filter(o=>o!==n.id)})}/>{n.id}</label>)}</div>}
   <button onClick={()=>edit(g=>removePopulation(g,node.id))}>Remove population</button>
  </fieldset>)}
  <button disabled={!undo.current.length} onClick={()=>{const value=undo.current.pop()!;redo.current.push(text);publish(value);}}>Undo draft edit</button>
  <button disabled={!redo.current.length} onClick={()=>{const value=redo.current.pop()!;undo.current.push(text);publish(value);}}>Redo draft edit</button>
 </fieldset><p>Draft only. Save a backend revision to persist. Native validation checks cycles, dependencies and bounds before evaluation/publication. Draft undo does not rewrite saved history.</p>{error&&<p role="alert">{error}</p>}</section>;
}
