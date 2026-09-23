import {useEffect,useRef,useState} from "react";
import {open,save} from "@tauri-apps/plugin-dialog";
import {bridge,type ReviewMetadata} from "./bridge";
import type {ReviewExportStatus} from "./reviewExport";

type Options = {source_kind?:"hdf"|"folder"|"avi";synthetic_background?:boolean;roi?:{x:number;y:number;w:number;h:number};image_processing?:unknown};

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
  async function start(source:string,dataset:string,start:number,count:number,options:Options={}){
    if(!ready || lock.current || current.current.state==="running")return;
    lock.current=true;setPending(true);setError("");
    try {
      if(!source || !Number.isSafeInteger(start) || start<0 || !Number.isSafeInteger(count) || count<0)throw new Error("Enter nonnegative integer range values.");
      const path=await save({title:"Save regenerated masks as a new HDF file",filters:[{name:"HDF5",extensions:["h5"]}],defaultPath:"regenerated.h5"});
      if(!path)return;
      if(!options.source_kind || options.source_kind==="hdf") {
        const metadata=await bridge.fetchReviewMetadata();
        if(!metadata.file_open || metadata.file_path!==source)throw new Error("Review source changed. Reopen the source before regenerating.");
      }
      const result=await bridge.reviewReanalysis({source_path:source,output_path:path,dataset,start,count,...options});
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
  const [dataset,setDataset]=useState("all"),[start,setStart]=useState("0"),[count,setCount]=useState("0");
  const [kind,setKind]=useState<"hdf"|"folder"|"avi">("hdf"),[external,setExternal]=useState("");
  const [synthetic,setSynthetic]=useState(true),[settings,setSettings]=useState(""),[roi,setRoi]=useState("");
  const [draftError,setDraftError]=useState("");
  useEffect(()=>{setDataset("all");setStart("0");setCount("0");},[metadata?.file_path,metadata?.recording_file]);
  async function choose(){
    try {const value=await open({title:kind==="folder"?"Choose image folder":"Choose AVI",directory:kind==="folder",multiple:false,...(kind==="avi"?{filters:[{name:"AVI",extensions:["avi"]}]}:{})});if(typeof value==="string")setExternal(value);}
    catch(e){setDraftError(String(e));}
  }
  async function snapshot(){
    try {const value=await bridge.fetchProcessingConfigJson();if(!value.valid)throw new Error("Current settings unavailable");setSettings(JSON.stringify(JSON.parse(value.json).image_processing,null,2));setDraftError("");}
    catch(e){setDraftError(String(e));}
  }
  async function run(){
    try {
      const options:Options={source_kind:kind,synthetic_background:synthetic};
      if(settings.trim())options.image_processing=JSON.parse(settings);
      if(roi.trim()) {
        const parts=roi.split(",").map(x=>Number(x.trim()));
        if(parts.length!==4 || parts.some(x=>!Number.isSafeInteger(x)) || parts[0]<0 || parts[1]<0 || parts[2]<=0 || parts[3]<=0)throw new Error("ROI requires x,y,width,height as nonnegative origin and positive integer size");
        options.roi={x:parts[0],y:parts[1],w:parts[2],h:parts[3]};
      }
      setDraftError("");await model.start(kind==="hdf"?metadata!.file_path:external,dataset,Number(start),Number(count),options);
    }catch(e){setDraftError(String(e));}
  }
  return <section className="config-group" aria-label="Regenerate saved masks">
    <h5>Regenerate masks / reanalyse</h5>
    <p>Saves a new HDF file; never overwrites the source. Current processing settings are copied unless a local settings draft is supplied. HDF source ROI/background and timestamps are preserved; folder/AVI inputs use full-frame ROI by default and generated timestamps. Active processing-core identity is recorded.</p>
    <label>Source kind <select value={kind} disabled={model.busy} onChange={e=>{setKind(e.target.value as typeof kind);setExternal("");}}><option value="hdf">Open HDF file</option><option value="folder">Image folder</option><option value="avi">AVI file</option></select></label>
    {kind!=="hdf" && <><button disabled={model.busy} onClick={()=>void choose()}>Choose source…</button><span>{external || "No source selected"}</span></>}
    {kind==="hdf" && <label>Source dataset <select value={dataset} onChange={e=>setDataset(e.target.value)} disabled={model.busy}><option value="all">Entire HDF (source order)</option><option value="/valid_frames/images">Valid frames</option><option value="/invalid_frames/images">Invalid frames</option><option value="/recorded_frames/images">Raw recording</option></select></label>}
    <p>Start is zero-based; count 0 selects the remaining dataset. Maximum 4096 frames / 256 MiB input per job.</p>
    <label>Start <input type="number" min="0" step="1" value={start} onChange={e=>setStart(e.target.value)} disabled={model.busy}/></label>
    <label>Count <input type="number" min="0" step="1" value={count} onChange={e=>setCount(e.target.value)} disabled={model.busy}/></label>
    <label>ROI override (x,y,width,height; blank uses source/full-frame) <input value={roi} onChange={e=>setRoi(e.target.value)} disabled={model.busy}/></label>
    <label><input type="checkbox" checked={synthetic} onChange={e=>setSynthetic(e.target.checked)} disabled={model.busy}/>Build synthetic background when source has none</label>
    <details><summary>Local processing-settings draft (does not change live settings)</summary><button disabled={model.busy} onClick={()=>void snapshot()}>Copy current settings</button><textarea aria-label="Reanalysis processing settings" value={settings} onChange={e=>setSettings(e.target.value)} disabled={model.busy}/></details>
    {draftError && <p role="alert">{draftError}</p>}
    <button disabled={(kind==="hdf"?!metadata?.file_open:!external) || model.busy || blocked} onClick={()=>void run()}>Regenerate into new HDF…</button>
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
