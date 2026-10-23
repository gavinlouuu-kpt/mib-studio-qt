import { useState } from "react";
import type { AnalysisCall } from "./AnalysisWorkspace";
import { AnalysisPlot,type PlotArtifact } from "./AnalysisPlot";
const journal="mib.analysis.pending-overlay.v1";
export function AnalysisOverlay({read,write,disabled}:{read:AnalysisCall;write:AnalysisCall;disabled:boolean}){
 const [id,setId]=useState("");const [revision,setRevision]=useState("");const [busy,setBusy]=useState(false);const [message,setMessage]=useState("");const [plot,setPlot]=useState<PlotArtifact|null>(null);
 const [spec,setSpec]=useState(JSON.stringify({title:"Retained source overlay",layers:[]},null,2));
 const checked=(reply:Record<string,any>)=>{if(reply.status!=="ok")throw new Error(`${reply.status}: ${reply.message||"Overlay refused"}`);return reply;};
 async function run(work:()=>Promise<void>){setBusy(true);setMessage("");try{await work();}catch(e){setMessage(String(e));}finally{setBusy(false);}}
 async function submit(command:Record<string,unknown>){const reply=checked(await write(command));if(window.localStorage.getItem(journal)===JSON.stringify(command))window.localStorage.removeItem(journal);setRevision(reply.revision);setMessage(`Overlay revision ${reply.revision} saved.`);}
 return <details><summary>Overlays across retained sources</summary>
  <p>Use exact saved histogram references with identical bins, normalization, units and preprocessing. Every layer keeps its own denominator. Offsets affect display only; visual comparison does not establish scientific compatibility or independent replicates.</p>
  <fieldset disabled={disabled||busy}>
   <label>Overlay ID<input value={id} onChange={e=>{setId(e.target.value);setRevision("");setPlot(null);}}/></label>
   <label>Exact overlay revision<input value={revision} onChange={e=>{setRevision(e.target.value);setPlot(null);}}/></label>
   <label>Overlay layers JSON<textarea aria-label="Multi-source overlay specification" rows={12} value={spec} onChange={e=>setSpec(e.target.value)}/></label>
   <p>Layer: <code>{'{"id":"before","label":"Before","population":"area-gate","displayOffset":0,"reference":{"planId":"plan","revision":"1","source":{"kind":"retained","id":"snapshot"}}}'}</code></p>
   <button disabled={!id} onClick={()=>void run(async()=>{if(window.localStorage.getItem(journal))throw new Error("Reconcile pending overlay first.");const command={action:"overlaySave",overlayId:id,expectedRevision:revision||null,commandId:crypto.randomUUID(),spec:JSON.parse(spec)};window.localStorage.setItem(journal,JSON.stringify(command));await submit(command);})}>Save overlay revision</button>
   <button disabled={!id||!revision} onClick={()=>void run(async()=>{const reply=checked(await read({action:"overlayRead",overlayId:id,revision}));setSpec(JSON.stringify(reply.document.spec,null,2));})}>Load exact overlay</button>
   <button disabled={!id||!revision} onClick={()=>void run(async()=>{const reply=checked(await read({action:"overlayEvaluate",overlayId:id,revision}));setPlot(reply.plot);})}>Render pinned multi-source overlay</button>
   <button onClick={()=>void run(async()=>{const pending=window.localStorage.getItem(journal);if(!pending)throw new Error("No pending overlay command");await submit(JSON.parse(pending));})}>Reconcile pending overlay</button>
  </fieldset><p role="status">{message}</p>{plot&&<AnalysisPlot artifact={plot}/>}
 </details>;
}
