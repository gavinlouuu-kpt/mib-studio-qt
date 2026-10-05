import {useEffect,useRef,useState} from "react";
import {open,save} from "@tauri-apps/plugin-dialog";
import {bridge,mono8ToImageData,type FramePacket,type ReviewMetadata} from "./bridge";
import type {ReviewExportStatus} from "./reviewExport";

type Options = {source_kind?:"hdf"|"folder"|"avi";synthetic_background?:boolean;roi?:{x:number;y:number;w:number;h:number};image_processing?:unknown;max_frames?:number;max_input_mib?:number;background_index?:number;background_dataset?:string;clear_background?:boolean};
type PreviewSpec={source_kind:string;source_path:string;dataset:string;index:number};

// Owned at App scope so reconciliation and cancellation survive navigation.
export function useReanalysis(ready:boolean) {
  const [dataset,setDataset]=useState("all"),[startIndex,setStart]=useState("0"),[count,setCount]=useState("0");
  const [kind,setKind]=useState<"hdf"|"folder"|"avi">("hdf"),[external,setExternal]=useState("");
  const [synthetic,setSynthetic]=useState(true),[settings,setSettings]=useState(""),[roi,setRoi]=useState("");
  const [draftError,setDraftError]=useState("");
  const [maxFrames,setMaxFrames]=useState("4096"),[maxInputMiB,setMaxInputMiB]=useState("256");
  const [previewIndex,setPreviewIndex]=useState("0"),[previewDataset,setPreviewDataset]=useState("/valid_frames/images");
  const [preview,setPreview]=useState<{spec:PreviewSpec;frame:FramePacket}|null>(null);
  const [background,setBackground]=useState<PreviewSpec|null>(null),[clearBackground,setClearBackground]=useState(false);
  const [previewPending,setPreviewPending]=useState(false);
  const previewLock=useRef(false);
  const sourceRef=useRef<string|null>(null);
  const [status,setStatus]=useState<ReviewExportStatus>({state:"idle"});
  const [pending,setPending]=useState(false);
  const [reconciled,setReconciled]=useState(false);
  const [error,setError]=useState("");
  const revision=useRef(0);
  const lock=useRef(false), current=useRef(status);current.current=status;
  useEffect(()=>{
    if(!ready){setReconciled(false);return;}
    let active=true;let timer:ReturnType<typeof setTimeout>;
    const poll=async()=>{
      const version=revision.current;
      try {const next=await bridge.reviewReanalysisStatus();if(active && version===revision.current){setStatus(next);setReconciled(true);}}
      catch(e){if(active)setError(`Reanalysis status unavailable: ${String(e)}`);}
      finally{if(active)timer=setTimeout(()=>void poll(),500);}
    };
    void poll();return()=>{active=false;clearTimeout(timer);};
  },[ready]);
  async function start(source:string,dataset:string,start:number,count:number,options:Options={}){
    if(!ready || !reconciled || lock.current || current.current.state==="running")return;
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
  async function loadPreview(spec:PreviewSpec){
    if(!ready || previewLock.current || lock.current)return;
    previewLock.current=true;setPreviewPending(true);setDraftError("");
    try{if(!Number.isSafeInteger(spec.index) || spec.index<0)throw new Error("Preview index must be a nonnegative integer");const frame=await bridge.fetchReanalysisPreview(spec);if(!frame.valid)throw new Error("Cannot preview this source/index");setPreview({spec,frame});}
    catch(e){setDraftError(String(e));}finally{previewLock.current=false;setPreviewPending(false);}
  }
  return {status,error,pending,start,cancel,loadPreview,previewPending,draft:{dataset,setDataset,start:startIndex,setStart,count,setCount,kind,setKind,external,setExternal,synthetic,setSynthetic,settings,setSettings,roi,setRoi,draftError,setDraftError,sourceRef,previewIndex,setPreviewIndex,previewDataset,setPreviewDataset,preview,setPreview,background,setBackground,clearBackground,setClearBackground,maxFrames,setMaxFrames,maxInputMiB,setMaxInputMiB},busy:(ready&&!reconciled) || pending || status.state==="running"};
}
export function ReanalysisControls({model,metadata,blocked=false}:{model:ReturnType<typeof useReanalysis>;metadata:ReviewMetadata|null;blocked?:boolean}) {
  const {dataset,setDataset,start,setStart,count,setCount,kind,setKind,external,setExternal,synthetic,setSynthetic,settings,setSettings,roi,setRoi,draftError,setDraftError,sourceRef,previewIndex,setPreviewIndex,previewDataset,setPreviewDataset,preview,setPreview,background,setBackground,clearBackground,setClearBackground,maxFrames,setMaxFrames,maxInputMiB,setMaxInputMiB}=model.draft;
  useEffect(()=>{const source=metadata?.file_path ?? "";if(sourceRef.current!==source){sourceRef.current=source;setDataset("all");setStart("0");setCount("0");setPreview(null);setBackground(null);setClearBackground(false);setPreviewDataset(metadata?.recording_file?"/recorded_frames/images":"/valid_frames/images");}},[metadata?.file_path,metadata?.recording_file]);
  const source=kind==="hdf"?metadata?.file_path ?? "":external;
  const previewSpec:PreviewSpec={source_kind:kind,source_path:source,dataset:previewDataset,index:Number(previewIndex)};
  const matchingPreview=preview && JSON.stringify(preview.spec)===JSON.stringify(previewSpec)?preview:null;
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
      if(!Number.isSafeInteger(Number(maxFrames)) || !Number.isSafeInteger(Number(maxInputMiB)) || Number(maxFrames)<1 || Number(maxFrames)>1000000 || Number(maxInputMiB)<1 || Number(maxInputMiB)>16384)throw new Error("Choose an integer input budget of 1–1000000 frames and 1–16384 MiB.");
      const options:Options={source_kind:kind,synthetic_background:synthetic,clear_background:clearBackground,max_frames:Number(maxFrames),max_input_mib:Number(maxInputMiB)};
      if(background){if(background.source_kind!==kind || background.source_path!==source)throw new Error("Selected background belongs to another source; clear or reselect it.");options.background_index=background.index;options.background_dataset=background.dataset;}
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
    <p>Start is zero-based; count 0 selects the remaining dataset. The default budget is 4096 frames / 256 MiB; increase the explicit job budget below for larger datasets.</p>
    <label>Start <input type="number" min="0" step="1" value={start} onChange={e=>setStart(e.target.value)} disabled={model.busy}/></label>
    <label>Count <input type="number" min="0" step="1" value={count} onChange={e=>setCount(e.target.value)} disabled={model.busy}/></label>
    <details><summary>Preview source, select background and draw ROI</summary>
      {kind==="hdf" && <label>Preview dataset <select value={previewDataset} onChange={e=>setPreviewDataset(e.target.value)}><option value="/valid_frames/images">Valid</option><option value="/invalid_frames/images">Invalid</option><option value="/recorded_frames/images">Raw recording</option></select></label>}
      <label>Preview index <input type="number" min="0" step="1" value={previewIndex} onChange={e=>setPreviewIndex(e.target.value)}/></label>
      <button disabled={!source || model.busy || model.previewPending} onClick={()=>void model.loadPreview(previewSpec)}>Load source preview</button>
      <button disabled={!matchingPreview || model.busy} onClick={()=>{setBackground(previewSpec);setClearBackground(false);}}>Use preview frame as background</button>
      <button disabled={model.busy} onClick={()=>{setBackground(null);setClearBackground(true);setSynthetic(false);}}>Clear background</button>
      <button disabled={model.busy} onClick={()=>{setBackground(null);setClearBackground(false);}}>Use saved source background</button>
      {background && <p>Selected background: {background.source_path} · {background.dataset} · index {background.index}</p>}
      {matchingPreview && <SourcePreview frame={matchingPreview.frame} roi={roi} onRoi={setRoi} disabled={model.busy}/>}
    </details>
    <label>ROI override (x,y,width,height; blank uses source/full-frame) <input value={roi} onChange={e=>setRoi(e.target.value)} disabled={model.busy}/></label>
    <label><input type="checkbox" checked={synthetic} onChange={e=>setSynthetic(e.target.checked)} disabled={model.busy}/>Build synthetic background when source has none</label>
    <details><summary>Input memory/frame budget</summary><p>This budget covers decoded inputs; processing masks/results require additional memory.</p><label>Maximum frames <input type="number" min="1" max="1000000" value={maxFrames} onChange={e=>setMaxFrames(e.target.value)} disabled={model.busy}/></label><label>Input MiB <input type="number" min="1" max="16384" value={maxInputMiB} onChange={e=>setMaxInputMiB(e.target.value)} disabled={model.busy}/></label></details>
    <details><summary>Local processing-settings draft (does not change live settings)</summary><button disabled={model.busy} onClick={()=>void snapshot()}>Copy current settings</button><textarea aria-label="Reanalysis processing settings" value={settings} onChange={e=>setSettings(e.target.value)} disabled={model.busy}/></details>
    {draftError && <p role="alert">{draftError}</p>}
    <button disabled={(kind==="hdf"?!metadata?.file_open:!external) || model.busy || blocked} onClick={()=>void run()}>Regenerate into new HDF…</button>
  </section>;
}

function SourcePreview({frame,roi,onRoi,disabled}:{frame:FramePacket;roi:string;onRoi:(value:string)=>void;disabled:boolean}) {
  const canvas=useRef<HTMLCanvasElement>(null),origin=useRef<[number,number]|null>(null);
  useEffect(()=>{
    const node=canvas.current;if(!node)return;node.width=frame.width;node.height=frame.height;
    const ctx=node.getContext("2d");if(!ctx)return;
    ctx.putImageData(mono8ToImageData(frame.data,frame.width,frame.height,frame.stride_bytes),0,0);
    const [x,y,w,h]=roi.split(",").map(Number);if([x,y,w,h].every(Number.isFinite) && w>0 && h>0){ctx.strokeStyle="#facc15";ctx.lineWidth=2;ctx.strokeRect(x,y,w,h);}
  },[frame,roi]);
  const position=(event:React.PointerEvent<HTMLCanvasElement>):[number,number]=>{const bounds=event.currentTarget.getBoundingClientRect();return [Math.max(0,Math.min(frame.width,Math.floor((event.clientX-bounds.left)/bounds.width*frame.width))),Math.max(0,Math.min(frame.height,Math.floor((event.clientY-bounds.top)/bounds.height*frame.height)))];};
  return <><p>{frame.width}×{frame.height}. Drag on the image to set the local ROI.</p><canvas ref={canvas} style={{maxWidth:"100%",touchAction:"none"}} aria-label="Reanalysis source preview" onPointerDown={event=>{if(disabled)return;origin.current=position(event);event.currentTarget.setPointerCapture(event.pointerId);}} onPointerUp={event=>{if(disabled || !origin.current)return;const end=position(event),start=origin.current;origin.current=null;const x=Math.min(start[0],end[0]),y=Math.min(start[1],end[1]),w=Math.abs(end[0]-start[0]),h=Math.abs(end[1]-start[1]);if(w && h)onRoi(`${x},${y},${w},${h}`);}}/></>;
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
