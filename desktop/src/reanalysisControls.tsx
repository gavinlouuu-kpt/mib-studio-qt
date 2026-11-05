// MIB Studio's Review-stage "Regenerate masks / reanalyse" panel: the Qt BatchMaskDialog's
// Input source | Preview & ROI | Processing Config groups (#867 REVIEW-057, -059..-064, -067).
// The preview/ROI/background and Processing Config groups are the components YOFO Review's
// dialog shares (review/regenerate/); this file owns the state, the bridge calls and the
// run. The hook is owned at App scope so reconciliation, cancellation and the draft survive
// navigation.
import {useEffect,useRef,useState} from "react";
import {open,save} from "./transport/dialogs";
import {bridge,mono8ToImageData,type ReviewMetadata} from "./bridge";
import type {ReviewExportStatus} from "./reviewExport";
import {ProcessingConfigGroup,RoiPreviewPanel,type PreviewFrame} from "./review/regenerate/RegeneratePanels";
import {
  DEFAULT_REGEN_CONFIG,backgroundLabel,configFromImageProcessing,coreMismatchText,imageProcessingOverride,joinDir,
  previewWindow,remaskedDefault,roiError,type RegenBackground,type RegenConfig,type Roi,
} from "./review/regenerate/regenerateForm";

type SourceKind="hdf"|"folder"|"avi";
type Options = {source_kind?:SourceKind;synthetic_background?:boolean;roi?:Roi;image_processing?:unknown;max_frames?:number;max_input_mib?:number;background_index?:number;background_dataset?:string;clear_background?:boolean};
/** Loads a finished job's file into the Review stage (useReview's openReviewPath): true once it is open. */
export type OpenReviewPath=(path:string)=>Promise<boolean>;

const VALID="/valid_frames/images",INVALID="/invalid_frames/images",RECORDED="/recorded_frames/images";
// The contract maxima (review_reanalysis_json): Qt's dialog has no cap, so neither does the default.
const BUDGET_FRAMES=1000000,BUDGET_MIB=16384;

const datasetInfo=(metadata:ReviewMetadata|null|undefined,dataset:string)=>dataset===RECORDED?metadata?.recorded_images:dataset===INVALID?metadata?.invalid_images:metadata?.valid_images;
const datasetCount=(metadata:ReviewMetadata|null|undefined,dataset:string)=>{const info=datasetInfo(metadata,dataset);return info?.present?Number(info.count)||0:0;};
/** The ROI an experiment file records (a recording records none): the dialog starts from it, as Qt does. */
const recordedRoi=(metadata:ReviewMetadata|null|undefined):Roi|null=>metadata?.file_open && !metadata.recording_file && metadata.roi_w>0 && metadata.roi_h>0?{x:metadata.roi_x,y:metadata.roi_y,w:metadata.roi_w,h:metadata.roi_h}:null;
/** What a source starts with: an HDF file its recorded ROI and background, folder/AVI the full frame and none. */
function sourceDefaults(kind:SourceKind,metadata:ReviewMetadata|null|undefined):{roi:Roi|null;background:RegenBackground}{
  return {roi:kind==="hdf"?recordedRoi(metadata):null,background:kind==="hdf" && metadata?.file_open && metadata.has_background?{kind:"recorded"}:{kind:"none"}};
}

// Owned at App scope so reconciliation and cancellation survive navigation. `onOpen` loads the
// regenerated file into Review once an operation started from this panel completes.
export function useReanalysis(ready:boolean,onOpen?:OpenReviewPath) {
  // The draft lives here, not in the panel, so leaving and returning to Review keeps it.
  const [kind,setKind]=useState<SourceKind>("hdf"),[folderPath,setFolderPath]=useState(""),[aviPath,setAviPath]=useState("");
  const [dataset,setDataset]=useState(VALID),[startIndex,setStart]=useState("0"),[count,setCount]=useState("100"),[entire,setEntire]=useState(true);
  const [synthetic,setSynthetic]=useState(true);
  const [roi,setRoi]=useState<Roi|null>(null),[background,setBackground]=useState<RegenBackground>({kind:"none"});
  const [previewIndex,setPreviewIndex]=useState(0);
  const [config,setLocalConfig]=useState<RegenConfig>(DEFAULT_REGEN_CONFIG),[configNote,setConfigNote]=useState("");
  const configEdited=useRef(false);
  const [draftError,setDraftError]=useState("");
  const [maxFrames,setMaxFrames]=useState(String(BUDGET_FRAMES)),[maxInputMiB,setMaxInputMiB]=useState(String(BUDGET_MIB));
  const sourceRef=useRef<string|null>(null);
  const [status,setStatus]=useState<ReviewExportStatus>({state:"idle"});
  const [pending,setPending]=useState(false);
  const [reconciled,setReconciled]=useState(false);
  const [error,setError]=useState("");
  const revision=useRef(0);
  const lock=useRef(false), current=useRef(status);current.current=status;
  // Operations started from this panel, and the ones already opened in Review (once each).
  const started=useRef(new Set<string>()),handled=useRef(new Set<string>());
  const [openedOperation,setOpenedOperation]=useState("");
  const openRef=useRef(onOpen);openRef.current=onOpen;
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
  // REVIEW-067: open the new file once per operation that this panel started.
  useEffect(()=>{
    const id=status.operation_id===undefined?"":String(status.operation_id);
    if(status.state!=="completed" || !status.final_path || !id || !started.current.has(id) || handled.current.has(id))return;
    handled.current.add(id);
    const opener=openRef.current;if(!opener)return;
    void Promise.resolve(opener(status.final_path)).then(ok=>{if(ok===true)setOpenedOperation(id);},()=>undefined);
  },[status]);
  async function start(source:string,dataset:string,start:number,count:number,options:Options={}){
    if(!ready || !reconciled || lock.current || current.current.state==="running")return;
    lock.current=true;setPending(true);setError("");
    try {
      if(!source || !Number.isSafeInteger(start) || start<0 || !Number.isSafeInteger(count) || count<0)throw new Error("Enter nonnegative integer range values.");
      // Qt's default: <base>_remasked.h5 beside an HDF5 or AVI source, inside an image folder.
      const output=remaskedDefault(options.source_kind ?? "hdf",source);
      const path=await save({title:"Save regenerated masks as a new HDF file",filters:[{name:"HDF5",extensions:["h5"]}],defaultPath:joinDir(output.dir,`${output.base}_remasked.h5`)});
      if(!path)return;
      if(!options.source_kind || options.source_kind==="hdf") {
        const metadata=await bridge.fetchReviewMetadata();
        if(!metadata.file_open || metadata.file_path!==source)throw new Error("Review source changed. Reopen the source before regenerating.");
      }
      const result=await bridge.reviewReanalysis({source_path:source,output_path:path,dataset,start,count,...options});
      if(!result.ok)throw new Error(result.message);
      started.current.add(String(result.operation_id));
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
  /** Processing Config: the live pipeline's eight fields (Qt resetConfigToLive). An edit made since the
   * last load stays unless `force` (the Reset button). */
  async function loadLiveConfig(force=false){
    const value=await bridge.fetchProcessingConfigJson();
    if(!value?.valid)throw new Error("Live processing settings are unavailable");
    const next=configFromImageProcessing(JSON.parse(value.json).image_processing);
    if(configEdited.current && !force)return;
    configEdited.current=false;setLocalConfig(next);setConfigNote("");
  }
  const setConfig=(next:RegenConfig)=>{configEdited.current=true;setLocalConfig(next);};
  return {status,error,pending,start,cancel,ready,openedOperation,loadLiveConfig,draft:{kind,setKind,folderPath,setFolderPath,aviPath,setAviPath,dataset,setDataset,start:startIndex,setStart,count,setCount,entire,setEntire,synthetic,setSynthetic,roi,setRoi,background,setBackground,previewIndex,setPreviewIndex,config,setConfig,configNote,setConfigNote,configEdited,draftError,setDraftError,sourceRef,maxFrames,setMaxFrames,maxInputMiB,setMaxInputMiB},busy:(ready&&!reconciled) || pending || status.state==="running"};
}

export function ReanalysisControls({model,metadata,blocked=false}:{model:ReturnType<typeof useReanalysis>;metadata:ReviewMetadata|null;blocked?:boolean}) {
  const d=model.draft,setDraftError=d.setDraftError;
  const fileOpen=!!metadata?.file_open,recording=!!metadata?.recording_file,recorded=recordedRoi(metadata);
  const filePath=fileOpen?metadata!.file_path:"";
  const mainDataset=recording?RECORDED:VALID;
  const hdf=d.kind==="hdf";
  const external=d.kind==="folder"?d.folderPath:d.kind==="avi"?d.aviPath:"";
  const source=hdf?filePath:external;

  // A different file: start over from its defaults (the draft otherwise survives leaving Review).
  useEffect(()=>{
    if(d.sourceRef.current===filePath)return;
    d.sourceRef.current=filePath;
    const kind:SourceKind=filePath?"hdf":d.kind==="hdf"?"folder":d.kind;
    const next=sourceDefaults(kind,metadata);
    d.setKind(kind);d.setDataset(mainDataset);d.setStart("0");d.setCount("100");d.setEntire(true);d.setPreviewIndex(0);
    d.setRoi(next.roi);d.setBackground(next.background);d.setDraftError("");
  },[filePath,recording]); // eslint-disable-line react-hooks/exhaustive-deps

  // The preview set (Qt getSourceFrameCount): the whole file reads its main dataset, a range the chosen one;
  // the facade cannot count a folder or an AVI, so those leave the end open until a frame fails.
  const previewDataset=hdf?(d.entire?mainDataset:d.dataset):"all";
  const win:{offset:number;total:number|null}=hdf?previewWindow(datasetCount(metadata,previewDataset),d.entire,Number(d.start),Number(d.count)):{offset:0,total:null};
  const total=win.total;
  const index=total===null?d.previewIndex:Math.min(d.previewIndex,Math.max(0,total-1));
  const spec=source && (total===null || total>0)?{source_kind:d.kind,source_path:source,dataset:previewDataset,index:win.offset+index}:null;
  const specKey=spec?JSON.stringify(spec):"";

  // Load the frame whenever the source, window or index changes. A response whose request is no longer
  // current is dropped (cleanup), and only a frame for the current request is ever offered.
  const [shot,setShot]=useState<{key:string;frame:PreviewFrame}|null>(null);
  const [failure,setFailure]=useState<{key:string;message:string}|null>(null);
  const [loadingKey,setLoadingKey]=useState("");
  useEffect(()=>{
    if(!spec || !model.ready || model.busy || shot?.key===specKey)return;
    let live=true;setLoadingKey(specKey);setFailure(null);
    void (async()=>{
      try {
        const packet=await bridge.fetchReanalysisPreview(spec);
        if(!live)return;
        if(!packet?.valid)throw new Error("Cannot preview this source/index");
        setShot({key:specKey,frame:{width:packet.width,height:packet.height,image:mono8ToImageData(packet.data,packet.width,packet.height,packet.stride_bytes)}});
      }catch(e){if(live)setFailure({key:specKey,message:String(e)});}
      finally{if(live)setLoadingKey("");}
    })();
    return()=>{live=false;};
  },[specKey,model.ready,model.busy]); // eslint-disable-line react-hooks/exhaustive-deps
  const frame=shot && shot.key===specKey?shot.frame:null;
  const previewError=failure && failure.key===specKey?failure.message:"";

  // Processing Config: start from the live pipeline unless the draft was edited.
  useEffect(()=>{
    if(!model.ready || d.configEdited.current)return;
    void model.loadLiveConfig().catch(e=>d.setConfigNote(`Live processing settings unavailable (${String(e)}); showing the kernel defaults.`));
  },[model.ready]); // eslint-disable-line react-hooks/exhaustive-deps

  // REVIEW-063: Qt warns on Run; here before it. The file's recorded core against the active one.
  const [coreText,setCoreText]=useState("");
  const coreKey=`${filePath}|${metadata?.has_core_identity}|${metadata?.core_version}|${metadata?.core_source}|${metadata?.core_release_tag}`;
  useEffect(()=>{
    setCoreText("");
    if(!model.ready || !filePath || !metadata?.has_core_identity)return;
    let live=true;
    void (async()=>{
      try {
        const active=await bridge.fetchProcessingCoreStatus();
        if(!live || !active?.valid)return;
        if(metadata.core_version!==active.active_version || metadata.core_source!==active.source || metadata.core_release_tag!==active.release_tag)
          setCoreText(coreMismatchText({version:metadata.core_version,contract:null},{version:active.active_version,contract:active.contract_version}));
      }catch{/* no status: no warning */}
    })();
    return()=>{live=false;};
  },[coreKey,model.ready]); // eslint-disable-line react-hooks/exhaustive-deps

  /** A different source (radio, Browse…): its preview, background and ROI start over. */
  function selectSource(kind:SourceKind,path?:string){
    if(path!==undefined){if(kind==="folder")d.setFolderPath(path);else if(kind==="avi")d.setAviPath(path);}
    const next=sourceDefaults(kind,metadata);
    d.setKind(kind);d.setPreviewIndex(0);d.setRoi(next.roi);d.setBackground(next.background);d.setDraftError("");
  }
  async function choose(kind:"folder"|"avi"){
    try {const value=await open({title:kind==="folder"?"Choose image folder":"Choose AVI",directory:kind==="folder",multiple:false,...(kind==="avi"?{filters:[{name:"AVI",extensions:["avi"]}]}:{})});if(typeof value==="string")selectSource(kind,value);}
    catch(e){d.setDraftError(String(e));}
  }
  // The range edits restart the preview at its first frame (Qt re-reads the set).
  const editRange=(set:(value:string)=>void,value:string)=>{set(value);d.setPreviewIndex(0);};
  const frameBackground=():RegenBackground|null=>spec?{kind:"frame",index:spec.index,dataset:hdf?spec.dataset:"",label:index+1}:null;

  const dims=frame?{width:frame.width,height:frame.height}:hdf?(()=>{const info=datasetInfo(metadata,previewDataset);return info?.present && info.width>0 && info.height>0?{width:info.width,height:info.height}:undefined;})():undefined;
  async function run(){
    try {
      const frames=Number(d.maxFrames),mib=Number(d.maxInputMiB);
      if(!Number.isSafeInteger(frames) || !Number.isSafeInteger(mib) || frames<1 || frames>BUDGET_FRAMES || mib<1 || mib>BUDGET_MIB)throw new Error(`Choose an integer input budget of 1–${BUDGET_FRAMES} frames and 1–${BUDGET_MIB} MiB.`);
      const problem=roiError(d.roi,dims?.width,dims?.height);if(problem)throw new Error(problem);
      const options:Options={source_kind:d.kind,synthetic_background:d.synthetic,max_frames:frames,max_input_mib:mib,image_processing:imageProcessingOverride(d.config)};
      if(d.roi && d.roi.w>0 && d.roi.h>0)options.roi=d.roi;
      // "Full frame" has to beat the ROI the file records: with no ROI sent the backend would use that one.
      else if(hdf && recorded && dims)options.roi={x:0,y:0,w:dims.width,h:dims.height};
      const bg=d.background;
      if(bg.kind==="frame"){options.clear_background=true;options.background_index=bg.index;if(bg.dataset)options.background_dataset=bg.dataset;}
      else if(bg.kind==="none" || !hdf)options.clear_background=true;
      setDraftError("");
      const range=hdf && !d.entire;
      await model.start(source,range?d.dataset:"all",range?Number(d.start):0,range?Number(d.count):0,options);
    }catch(e){setDraftError(String(e));}
  }
  const rangeOff=!hdf || d.entire || !fileOpen || model.busy;

  return <section className="config-group" aria-label="Regenerate saved masks">
    <h5>Regenerate masks / reanalyse</h5>
    <p>Saves the regenerated masks into a new HDF file; the source is never changed and an existing file is never overwritten. HDF sources keep their timestamps; folder and AVI inputs get generated ones. The active processing-core identity is recorded.</p>
    <div className="regen-columns">
      <fieldset className="regen-group" data-tour="studio-regen-source">
        <legend>Input source</legend>
        <label className="radio-row"><input type="radio" name="regen-source" checked={hdf} disabled={!fileOpen || model.busy} onChange={()=>selectSource("hdf")}/> Current HDF5 frames{fileOpen?` (${mainDataset})`:""}</label>
        <div className="regen-roi">
          <label>Frames <select aria-label="Range dataset" value={d.dataset} disabled={rangeOff} onChange={e=>editRange(d.setDataset,e.target.value)}><option value={VALID}>Valid</option><option value={INVALID}>Invalid</option><option value={RECORDED}>Raw recording</option></select></label>
          <label>Start <input type="number" min="0" step="1" aria-label="Start (0-based)" value={d.start} disabled={rangeOff} onChange={e=>editRange(d.setStart,e.target.value)}/></label>
          <label>Count <input type="number" min="0" step="1" aria-label="Count (0 = to the end)" value={d.count} disabled={rangeOff} onChange={e=>editRange(d.setCount,e.target.value)}/></label>
        </div>
        <label className="check-row"><input type="checkbox" checked={d.entire} disabled={!hdf || !fileOpen || model.busy} onChange={e=>{d.setEntire(e.target.checked);d.setPreviewIndex(0);}}/> Regenerate entire HDF5 file</label>
        <label className="radio-row"><input type="radio" name="regen-source" checked={d.kind==="folder"} disabled={model.busy} onChange={()=>selectSource("folder")}/> Folder of TIFF/PNG/JPEG images</label>
        <div className="regen-bg"><input type="text" readOnly aria-label="Image folder" placeholder="No folder chosen" value={d.folderPath} style={{flex:1,minWidth:0}}/><button className="btn" disabled={model.busy} onClick={()=>void choose("folder")}>Browse…</button></div>
        <label className="radio-row"><input type="radio" name="regen-source" checked={d.kind==="avi"} disabled={model.busy} onChange={()=>selectSource("avi")}/> AVI video file</label>
        <div className="regen-bg"><input type="text" readOnly aria-label="AVI video file" placeholder="No AVI chosen" value={d.aviPath} style={{flex:1,minWidth:0}}/><button className="btn" disabled={model.busy} onClick={()=>void choose("avi")}>Browse…</button></div>
        <p className="hint">Start is zero-based; a count of 0 takes the rest of the dataset.</p>
      </fieldset>
      <RoiPreviewPanel
        frame={frame} loading={!!spec && loadingKey===specKey && !model.busy} error={previewError}
        index={index} total={total} onIndex={d.setPreviewIndex}
        roi={d.roi} onRoi={d.setRoi} roiNote={hdf && recorded && d.roi && JSON.stringify(d.roi)===JSON.stringify(recorded)?"recorded in the file":undefined}
        backgroundLabel={backgroundLabel(d.background,d.synthetic)}
        onSetBackground={()=>{const next=frameBackground();if(next)d.setBackground(next);}}
        onClearBackground={()=>d.setBackground({kind:"none"})}
        extraBackground={hdf && metadata?.has_background?<button className="btn" disabled={model.busy} onClick={()=>d.setBackground({kind:"recorded"})}>Use saved</button>:undefined}
        synthetic={d.synthetic} onSynthetic={d.setSynthetic} disabled={model.busy}
      />
      <ProcessingConfigGroup
        config={d.config} onChange={d.setConfig}
        onReset={()=>void model.loadLiveConfig(true).catch(e=>setDraftError(`Cannot reset: ${String(e)}`))}
        resetLabel="Reset to live defaults"
        note="Processing parameters are local to this run. Use “Reset to live defaults” to re-sync with the live pipeline."
        disabled={model.busy}
      />
    </div>
    {d.configNote && <p className="hint" role="status">{d.configNote}</p>}
    {hdf && coreText && <p className="regen-warning" role="alert">{coreText}</p>}
    <p className="regen-calibration">{hdf
      ?"Objects are validated at the file's recorded px→µm factor (the live factor when the file records none) and E with the source run's E-LUT."
      :"Folder and AVI sources record no px→µm factor: objects are validated at the live factor."}</p>
    <details><summary>Input memory/frame budget</summary><p>The most frames and decoded MiB one run may load; the contract maxima by default (Qt sets no cap). Processing masks and results need additional memory.</p><label>Maximum frames <input type="number" min="1" max={BUDGET_FRAMES} value={d.maxFrames} onChange={e=>d.setMaxFrames(e.target.value)} disabled={model.busy}/></label><label>Input MiB <input type="number" min="1" max={BUDGET_MIB} value={d.maxInputMiB} onChange={e=>d.setMaxInputMiB(e.target.value)} disabled={model.busy}/></label></details>
    {d.draftError && <p role="alert">{d.draftError}</p>}
    <button data-tour="studio-regen-run" disabled={(hdf?!fileOpen:!external) || model.busy || blocked} onClick={()=>void run()}>Regenerate into new HDF…</button>
  </section>;
}

/** The status line (#763 F18). The backend's terminal status carries no
 * phase or progress, so printing those fields gave "Reanalysis: completed /";
 * each state now says what it knows: progress while running, the records
 * written when completed (the file is the "Output:" line), and plainly when a
 * completed job names no file. */
export function reanalysisStatusLine(status:ReviewExportStatus,pending:boolean):string{
  if(pending)return "Reanalysis: request pending";
  const count=(v:string|undefined)=>v!==undefined && v!=="" && Number.isFinite(Number(v))?Number(v):null;
  switch(status.state){
    case "running":{
      if(status.phase==="loading")return "Reanalysis: running · loading the source";
      const done=count(status.completed),total=count(status.total);
      return done!==null && total!==null?`Reanalysis: running · ${done} of ${total} frames processed`:"Reanalysis: running";
    }
    case "completed":{
      const objects=count(status.objects),valid=count(status.valid_objects);
      const records=objects===null?"":valid===null?` · ${objects} records`:` · ${objects} records (${valid} valid, ${Math.max(0,objects-valid)} invalid)`;
      // The path itself is the "Output:" line below the status.
      return status.final_path?`Reanalysis: completed${records}`:`Reanalysis: completed${records}, but the backend named no output file`;
    }
    case "cancelled":return "Reanalysis: cancelled";
    case "failed":return "Reanalysis: failed";
    default:return "Reanalysis: idle";
  }
}

/** REVIEW-064: what a completed job validated at (the px→µm factor and where it came from) and
 * computed E with. "" when the backend named no factor. */
export function reanalysisFactorLine(status:ReviewExportStatus):string{
  const factor=status.pixel_to_micron;
  if(status.state!=="completed" || typeof factor!=="number" || !Number.isFinite(factor) || factor<=0)return "";
  const source=status.pixel_to_micron_source==="recorded"?"recorded in the source file":status.pixel_to_micron_source==="fallback"?"live factor: the source records none":status.pixel_to_micron_source==="operator"?"operator":"";
  return `Validated at ${factor.toFixed(4)} µm/px${source?` (${source})`:""}${status.emodulus_lut?` · ${status.emodulus_lut}`:""}`;
}

export function ReanalysisStatus({model}:{model:ReturnType<typeof useReanalysis>}){
  if(model.status.state==="idle" && !model.error && !model.pending)return null;
  const opened=!!model.status.operation_id && model.openedOperation===String(model.status.operation_id);
  const factor=reanalysisFactorLine(model.status);
  return <section aria-label="Reanalysis status"><p role="status">{reanalysisStatusLine(model.status,model.pending)}</p>
    {model.status.state==="running" && <button disabled={model.pending} onClick={()=>void model.cancel()}>Cancel Reanalysis</button>}
    {model.status.final_path && <p>Output: {model.status.final_path}. {opened?"Opened in Review.":"Open this file in Review to inspect regenerated masks."}</p>}
    {factor && <p className="regen-calibration">{factor}</p>}
    {model.status.retained_partial_path && <p>Temporary file retained: {model.status.retained_partial_path}</p>}
    {(model.error || model.status.error) && <p role="alert">{model.error || model.status.error}</p>}
    {model.status.warnings?.map((warning,i)=><p key={i}>{warning}</p>)}
  </section>;
}
