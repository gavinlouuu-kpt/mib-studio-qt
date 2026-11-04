import {useEffect,useRef,useState} from "react";
import type {AnalysisCall} from "./AnalysisWorkspace";
/** Native revisions own progress; UI drives bounded steps, never fabricates report artifacts. */
export function AnalysisReportBatch({read,write,disabled}:{read:AnalysisCall;write:AnalysisCall;disabled:boolean}){
 const [id,setId]=useState(""),[pins,setPins]=useState("[]"),[batch,setBatch]=useState<Record<string,any>|null>(null);
 const [busy,setBusy]=useState(false),[message,setMessage]=useState("");
 const cancel=useRef(false),pending=useRef<Record<string,unknown>|null>(null);
 useEffect(()=>()=>{cancel.current=true;},[]);
 const checked=(r:Record<string,any>)=>{if(r.status!=="ok")throw Error(`${r.status}: ${r.message||"Batch refused; refresh progress"}`);return r;};
 async function run(work:()=>Promise<void>){setBusy(true);setMessage("");try{await work();}catch(e){setMessage(String(e));}finally{setBusy(false);}}
 async function refresh(){const r=checked(await read({action:"reportBatchRead",batchId:id}));setBatch(r);return r;}
 async function progress(all:boolean){
  cancel.current=false;let current=await refresh();
  do {
   if(["completed","cancelled"].includes(current.document.state))break;
   current=checked(await write({action:cancel.current?"reportBatchCancel":"reportBatchAdvance",batchId:id,revision:current.revision}));setBatch(current);
   if(current.document.state==="failed"&&!cancel.current)break;
  }while(all||cancel.current);
 }
 return <details><summary>Durable report batches</summary>
  <p>Queue 1–16 exact report layout revisions. Each step publishes one immutable report. Progress survives reconnect/restart; nothing runs automatically on restart. Cancellation stops subsequent reports after the current bounded build returns; completed artifacts remain. This is not an atomic all-report publication.</p>
  <fieldset disabled={disabled||busy}>
   <label>Report batch ID<input value={id} onChange={e=>{setId(e.target.value);setBatch(null);pending.current=null;}}/></label>
   <label>Exact report pins<textarea aria-label="Exact report pins" value={pins} onChange={e=>{setPins(e.target.value);pending.current=null;}} rows={4}/></label>
   <button disabled={!id} onClick={()=>void run(async()=>{pending.current??={action:"reportBatchCreate",batchId:id,commandId:crypto.randomUUID(),reports:JSON.parse(pins)};const r=checked(await write(pending.current));setBatch(r);})}>Create or reconcile report batch</button>
   <button disabled={!id} onClick={()=>void run(async()=>{await refresh();})}>Refresh report batch</button>
   <button disabled={!id} onClick={()=>void run(()=>progress(false))}>Advance one report</button>
   <button disabled={!id} onClick={()=>void run(()=>progress(true))}>Run remaining reports</button>
   <button disabled={!id} onClick={()=>void run(async()=>{const r=await refresh();setBatch(checked(await write({action:"reportBatchCancel",batchId:id,revision:r.revision})));})}>Cancel queued reports</button>
  </fieldset>
  {busy&&<button onClick={()=>{cancel.current=true;setMessage("Cancellation requested; the current report may finish. No later report will be started by this controller.");}}>Stop after current report</button>}
  {batch&&<p>Batch {id}, revision {batch.revision}: {batch.document.state}; {batch.document.next}/{batch.document.runs.length} reports complete. {batch.document.failure&&JSON.stringify(batch.document.failure)}</p>}
  {batch&&<ul>{batch.document.results.map((r:Record<string,any>)=><li key={r.runId}>Run {r.runId}, revision {r.revision}, hash {r.contentHash}</li>)}</ul>}
  <p role="status">{message}</p>
 </details>;
}
