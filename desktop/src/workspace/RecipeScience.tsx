import { useEffect, useRef, useState } from "react";
import { Button, Card, Notice } from "@mib/ui";
import { invoke } from "../transport";
import { mintExperimentId } from "./planModel";
import type { Recipe } from "./recipeModel";
export type Finding = {id:string; interpretation:string; recipe:Recipe; source_id:string; source_sha256:string; evaluation:Record<string,unknown>};
export function RecipeScience({workspaceId, revision, recipe, sources, controlHeld, blocked, profileBase, onOpenSource, onSaveFinding}: {
  workspaceId:string;revision:string;recipe:Recipe;
  sources:Record<string,{path:string;sha256:string;size_bytes:string}>;
  controlHeld:boolean;blocked:boolean;profileBase:string;
  onOpenSource?:(path:string)=>Promise<void>;
  onSaveFinding:(finding:Finding)=>void;
}) {
  const [sourceId,setSourceId]=useState("");
  const [kind,setKind]=useState("stored");
  const [busy,setBusy]=useState(false);
  const [message,setMessage]=useState("");
  const [result,setResult]=useState<Record<string,unknown>|null>(null);
  const [interpretation,setInterpretation]=useState("");
  const context=JSON.stringify([workspaceId,revision,recipe,sourceId,kind,profileBase]);
  const current=useRef(context);current.current=context;
  const lock=useRef(false);
  useEffect(()=>{setResult(null);setMessage("");},[context]);
  const source=sources[sourceId];
  const run=async(open:boolean)=>{
    if(lock.current||blocked||!source||(open&&!controlHeld))return;
    lock.current=true;setBusy(true);setResult(null);setMessage("");const key=context;
    try {
      if(open){await onOpenSource?.(source.path);if(current.current===key)setMessage("Source open requested through the existing Review workflow. Evaluate explicitly; a failed open will be refused by the source authority.");return;}
      const reply=await invoke<Record<string,unknown>>("workspace_read",{request:JSON.stringify({action:"evaluate_recipe",workspace_id:workspaceId,revision,source_id:sourceId,source_kind:kind,profile_base:profileBase})});
      if(current.current!==key)return;
      if(reply.status!=="ok")throw new Error(String(reply.message??reply.status));
      if(reply.workspace_id!==workspaceId||reply.revision!==revision||reply.source_id!==sourceId||!reply.evaluation||typeof reply.evaluation!=="object")throw new Error("Mismatched recipe evaluation identity");
      const e=reply.evaluation as Record<string,unknown>;
      if(e.planId!==recipe.analysis_plan.id||e.revision!==recipe.analysis_plan.revision||e.schema!=="F-evaluation/1")throw new Error("Mismatched pinned analysis revision");
      for(const name of ["selected","excluded","unknown","measured","missing"])if(typeof e[name]!=="string"||!/^\d+$/.test(e[name] as string))throw new Error("Malformed scientific count");
      for(const name of ["mean","median","minimum","maximum"])if(e[name]!==null&&(typeof e[name]!=="number"||!Number.isFinite(e[name])))throw new Error("Malformed scientific statistic");
      setResult(e);setMessage("Exact saved strategy evaluated against the verified source. Method resolution does not establish acquisition compatibility or hardware readiness.");
    }catch(error){if(current.current===key)setMessage(String(error));}
    finally{lock.current=false;setBusy(false);}
  };
  return <Card title="Pinned recipe analysis">
    <p>Resolve and evaluate the saved analysis revision through the native scientific provider. No frontend computation, method apply or acquisition start.</p>
    <label>Linked source <select aria-label="Recipe source" value={sourceId} disabled={busy||blocked} onChange={e=>setSourceId(e.target.value)}><option value="">Choose source</option>{Object.keys(sources).map(id=><option key={id} value={id}>{id}</option>)}</select></label>
    <label>Source observations <select aria-label="Recipe source kind" value={kind} disabled={busy||blocked} onChange={e=>setKind(e.target.value)}><option value="stored">Stored observations</option><option value="run-archive">E run evidence archive</option></select></label>
    {onOpenSource&&<Button disabled={busy||blocked||!source||!controlHeld} onClick={()=>void run(true)}>Open linked source in Review</Button>}
    <Button disabled={busy||blocked||!source} onClick={()=>void run(false)}>Evaluate pinned recipe</Button>
    {message&&<Notice tone="info" title="Scientific provider">{message}</Notice>}
    {result&&<><dl>{["selected","excluded","unknown","measured","missing","denominator","feature","unit","mean","median","minimum","maximum"].map(key=><div key={key}><dt>{key}</dt><dd>{result[key]===null||result[key]===undefined?"Unavailable":String(result[key])}</dd></div>)}</dl>
      <p>Population completeness: {result.populationComplete===true?"complete for this source snapshot":"not established; denominator unavailable"}. Scientific QC and independent replication are not inferred.</p>
      <label>Interpretation <textarea aria-label="Finding interpretation" value={interpretation} onChange={e=>setInterpretation(e.target.value)}/></label>
      <Button disabled={busy||blocked||!controlHeld} onClick={()=>onSaveFinding({id:mintExperimentId(),interpretation,recipe:structuredClone(recipe),source_id:sourceId,source_sha256:source.sha256,evaluation:structuredClone(result)})}>Save finding in workspace</Button></>}
  </Card>;
}
