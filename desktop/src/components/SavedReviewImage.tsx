import {useEffect,useRef,useState} from "react";
import {bridge,type ReviewMetadata} from "../bridge";
type Request={source_path:string;valid:boolean;index:number;mode:number;roi:boolean};
/** Independent saved-file render: image, mask and classification read together natively. */
export function SavedReviewImage({metadata,valid,index,fit=true}:{metadata:ReviewMetadata;valid:boolean;index:number;fit?:boolean}) {
  const [mode,setMode]=useState(0),[roi,setRoi]=useState(true),[url,setUrl]=useState("");
  const [error,setError]=useState(""),[loading,setLoading]=useState(false),[large,setLarge]=useState(false),[zoom,setZoom]=useState(1);
  const desired=useRef<Request|null>(null),working=useRef(false),generation=useRef(0),objectUrl=useRef("");
  const hasMask=(valid?metadata.valid_masks:metadata.invalid_masks).present;
  const hasRoi=metadata.roi_w>0 && metadata.roi_h>0;
  useEffect(()=>{
    const tag=++generation.current;
    const request={source_path:metadata.file_path,valid,index,mode:hasMask?mode:0,roi:roi&&hasRoi};
    desired.current=request;setLoading(true);setError("");setUrl("");
    if(objectUrl.current){URL.revokeObjectURL(objectUrl.current);objectUrl.current="";}
    const pump=async()=>{
      if(working.current)return;
      working.current=true;
      while(desired.current){
        const next=desired.current,revision=generation.current;desired.current=null;
        try{
          const bytes=await bridge.renderReviewOverlay(next);
          if(revision!==generation.current)continue;
          if(!bytes.length)throw new Error("Saved image render is empty");
          const value=URL.createObjectURL(new Blob([bytes.slice().buffer as ArrayBuffer],{type:"image/png"}));
          objectUrl.current=value;setUrl(value);setError("");
        }catch(e){if(revision===generation.current)setError(String(e));}
        finally{if(revision===generation.current)setLoading(false);}
      }
      working.current=false;
    };
    void pump();
    return()=>{if(generation.current===tag){++generation.current;desired.current=null;}if(objectUrl.current){URL.revokeObjectURL(objectUrl.current);objectUrl.current="";}};
  },[metadata.file_path,valid,index,mode,roi,hasMask,hasRoi]);
  const width=(valid?metadata.valid_images:metadata.invalid_images).width;
  return <section aria-label="Saved image overlays" style={large?{position:"fixed",inset:20,zIndex:1000,background:"#18212b",padding:16,overflow:"auto"}:undefined}>
    <div className="toolbar">
      <label>Overlay <select value={hasMask?mode:0} onChange={e=>setMode(Number(e.target.value))}><option value="0">None</option><option value="1" disabled={!hasMask}>All contours</option><option value="2" disabled={!hasMask}>Outer / inner contours</option><option value="3" disabled={!hasMask}>All mask</option><option value="4" disabled={!hasMask}>Filtered mask</option></select></label>
      <label><input type="checkbox" checked={roi} disabled={!hasRoi} onChange={e=>setRoi(e.target.checked)}/>Saved ROI</label>
      <button onClick={()=>setLarge(value=>!value)}>{large?"Close large viewer":"Open large viewer"}</button>
      {large && <label>Zoom <input aria-label="Saved-image zoom" type="range" min="0.25" max="4" step="0.25" value={zoom} onChange={e=>setZoom(Number(e.target.value))}/></label>}
    </div>
    {!hasMask && <p>No saved mask for this frame class; original image remains available.</p>}
    {loading && <p role="status">Rendering saved image…</p>}{error && <p role="alert">{error}</p>}
    {url && <img src={url} alt={`${valid?"Valid":"Invalid"} saved image ${index+1} with selected processing overlay`} style={{maxWidth:fit&&!large?"100%":"none",width:large?width*zoom:undefined,imageRendering:"pixelated"}}/>}
    <p>Blue: target · green: valid · red: invalid. Image, mask and classification come from the same saved source/index.</p>
  </section>;
}
