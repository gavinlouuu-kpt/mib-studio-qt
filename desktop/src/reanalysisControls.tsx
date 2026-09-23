import {useEffect,useRef,useState} from "react";
import {save} from "@tauri-apps/plugin-dialog";
import {bridge,type ReviewMetadata} from "./bridge";
import type {ReviewExportStatus} from "./reviewExport";

// Owned at App scope so reconciliation and cancellation survive navigation.
export function useReanalysis(ready:boolean) {
  const [status,setStatus]=useState<ReviewExportStatus>({state:"idle"});
  const [pending,setPending]=useState(false);
  const [error,setError]=useState("");
  const revision=useRef(0);
  const lock=useRef(false), current=useRef(status);current.current=status;
  useEffect(()=>{
    if(!ready)return;
    let active=true;let timer:ReturnType<typeof setTimeout>;
    const poll=async()=>{
      const version=revision.current;
      try {const next=await bridge.reviewReanalysisStatus();if(active && version===revision.current)setStatus(next);}
      catch(e){if(active)setError(`Reanalysis status unavailable: ${String(e)}`);}
      finally{if(active)timer=setTimeout(()=>void poll(),500);}
    };
    void poll();return()=>{active=false;clearTimeout(timer);};
  },[ready]);
  async function start(source:string,dataset:string,start:number,count:number){
    if(!ready || lock.current || current.current.state==="running")return;
    lock.current=true;setPending(true);setError("");
    try {
      if(!source || !Number.isSafeInteger(start) || start<0 || !Number.isSafeInteger(count) || count<0)throw new Error("Enter nonnegative integer range values.");
      const path=await save({title:"Save regenerated masks as a new HDF file",filters:[{name:"HDF5",extensions:["h5"]}],defaultPath:"regenerated.h5"});
      if(!path)return;
      const metadata=await bridge.fetchReviewMetadata();
      if(!metadata.file_open || metadata.file_path!==source)throw new Error("Review source changed. Reopen the source before regenerating.");
      const result=await bridge.reviewReanalysis({source_path:source,output_path:path,dataset,start,count});
      if(!result.ok)throw new Error(result.message);
      // Publish accepted operation immediately; a stale idle status cannot allow
      // a second submission while the first authoritative poll is in flight.
      ++revision.current;
      setStatus({state:"running",operation_id:result.operation_id});
      current.current={state:"running",operation_id:result.operation_id};
    }catch(e){setError(String(e));}
    finally{lock.current=false;setPending(false);}
  }
  async function cancel(){
    if(lock.current || current.current.state!=="running" || !current.current.operation_id)return;
    lock.current=true;setPending(true);
    try {const result=await bridge.cancelOperation(current.current.operation_id);if(!result.ok)setError(result.message);}
    catch(e){setError(String(e));}finally{lock.current=false;setPending(false);}
  }
  return {status,error,pending,start,cancel,busy:pending || status.state==="running"};
}
export function ReanalysisControls({model,metadata,blocked=false}:{model:ReturnType<typeof useReanalysis>;metadata:ReviewMetadata|null;blocked?:boolean}) {
  const [dataset,setDataset]=useState("/valid_frames/images"),[start,setStart]=useState("0"),[count,setCount]=useState("0");
  useEffect(()=>{setDataset(metadata?.recording_file?"/recorded_frames/images":"/valid_frames/images");setStart("0");setCount("0");},[metadata?.file_path,metadata?.recording_file]);
  return <section className="config-group" aria-label="Regenerate saved masks">
    <h5>Regenerate masks / reanalyse HDF</h5>
    <p>Uses current processing settings and the source ROI/background; saves a new file, never overwrites the source. The active processing core is recorded. Start is zero-based; count 0 selects the remaining dataset (maximum 4096 frames / 256 MiB input).</p>
    <label>Source dataset <select value={dataset} onChange={e=>setDataset(e.target.value)} disabled={model.busy}><option value="/valid_frames/images">Valid frames</option><option value="/invalid_frames/images">Invalid frames</option><option value="/recorded_frames/images">Raw recording</option></select></label>
    <label>Start <input type="number" min="0" step="1" value={start} onChange={e=>setStart(e.target.value)} disabled={model.busy}/></label>
    <label>Count <input type="number" min="0" step="1" value={count} onChange={e=>setCount(e.target.value)} disabled={model.busy}/></label>
    <button disabled={!metadata?.file_open || model.busy || blocked} onClick={()=>void model.start(metadata!.file_path,dataset,Number(start),Number(count))}>Regenerate into new HDF…</button>
  </section>;
}
export function ReanalysisStatus({model}:{model:ReturnType<typeof useReanalysis>}){
  if(model.status.state==="idle" && !model.error && !model.pending)return null;
  return <section aria-label="Reanalysis status"><p role="status">Reanalysis: {model.pending?"request pending":model.status.state} {model.status.phase} {model.status.completed}/{model.status.total}</p>
    {model.status.state==="running" && <button disabled={model.pending} onClick={()=>void model.cancel()}>Cancel Reanalysis</button>}
    {model.status.final_path && <p>Output: {model.status.final_path}. Open this file in Review to inspect regenerated masks.</p>}
    {model.status.retained_partial_path && <p>Temporary file retained: {model.status.retained_partial_path}</p>}
    {(model.error || model.status.error) && <p role="alert">{model.error || model.status.error}</p>}
    {model.status.warnings?.map((warning,i)=><p key={i}>{warning}</p>)}
  </section>;
}
