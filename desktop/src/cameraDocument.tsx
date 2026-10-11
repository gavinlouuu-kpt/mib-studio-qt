import {useEffect,useRef,useState} from "react";
import {invoke} from "./transport";
import {open,save,confirm} from "./transport/dialogs";
import {bridge} from "./bridge";
import {CAMERA_SELECTION_MODES} from "./bridgeContract";
import type {CameraScriptContext} from "./cameraScript";
import {QUALIFIED_NOTE,discardQuestion,rateSummary,strobeSummary,type PzCameraCheck,type RunFpsStatus} from "./runFps";
type Document={path:string;text:string;revision:string};
export type CameraDocumentKind="js"|"json"|"pz7035";
/** The PZ7035 camera document sets the Run rate and exposure: the instrument's current Run status (for the discard question) and the frames a held ring holds. */
export interface PzDocumentContext {status:RunFpsStatus|null;ringFrames:number}
export function useCameraDocument(context:CameraScriptContext,kind:CameraDocumentKind,serviceArmed=false,pz?:PzDocumentContext) {
 const [doc,setDoc]=useState<Document|null>(null),[text,setText]=useState(""),[dirty,setDirty]=useState(false),[busy,setBusy]=useState(false),[message,setMessage]=useState("");
 const pending=useRef(false),current=useRef(context);current.current=context;
 const pzRef=useRef(pz);pzRef.current=pz;
 // The PZ7035 document is checked by the backend as it is edited (no side effects): the editor shows the timing it gives, or the first problem.
 const [check,setCheck]=useState<PzCameraCheck|null>(null);
 useEffect(()=>{
  if(kind!=="pz7035"||!doc){setCheck(null);return;}
  let live=true;
  const timer=window.setTimeout(()=>{bridge.checkPzCameraDocument(text).then(r=>{if(live)setCheck(r);}).catch(e=>{if(live)setCheck({ok:false,error:String(e)});});},250);
  return ()=>{live=false;window.clearTimeout(timer);};
 },[kind,doc,text]);
 const run=async(action:"browse"|"reload"|"save"|"apply"|"clear"|"saveAs"|"default"|"trigger")=>{
  if(pending.current)return;
  if(["browse","reload","clear","default"].includes(action)&&dirty&&!await confirm("Discard unsaved camera document edits?"))return;
  pending.current=true;setBusy(true);setMessage("");
  try {
   if(action==="trigger") {
    if(kind!=="json" || !serviceArmed || !current.current.ready || !current.current.running || current.current.experimentActive)throw new Error("Arm Service mode with a running MindVision camera and idle experiment first.");
    const result=await invoke<{ok:boolean;message:string}>("soft_trigger_camera");if(!result.ok)throw new Error(result.message);setMessage(result.message);return;
   }
   if(action==="default") {const loaded=await invoke<Document>("camera_document",{action:"default",path:"",kind,baseline:"",text:""});setDoc(loaded);setText(loaded.text);setDirty(true);setMessage("Bundled default copied to draft; use Save As before applying.");return;}
   if(action==="saveAs") {
    const path=await save({defaultPath:`camera.${kind==="pz7035"?"json":kind}`,filters:[{name:"Camera document",extensions:[kind==="pz7035"?"json":kind]}]});if(!path)return;
    let existing:Document|null=null;
    try {existing=await invoke<Document>("camera_document",{action:"read",path,kind,baseline:"",text:""});}catch{/* Native create rejects collisions or unreadable existing paths. */}
    if(existing&&!await confirm("Replace this camera file with the current draft?"))return;
    const loaded=await invoke<Document>("camera_document",{action:existing?"save":"create",path,kind,baseline:existing?.revision??"",text});setDoc(loaded);setText(loaded.text);setDirty(false);setMessage("Saved copy; not applied to camera.");return;
   }
   if(action==="clear"){setDoc(null);setText("");setDirty(false);return;}
   if(action==="apply"&&kind==="pz7035"){
    if(!doc?.path || dirty)throw new Error("Save the draft before applying camera settings.");
    const ctx=current.current;
    if(!ctx.ready||ctx.experimentActive)throw new Error("Finalize the experiment before applying camera settings.");
    const disk=await invoke<Document>("camera_document",{action:"read",path:doc.path,kind,baseline:"",text:""});
    if(disk.revision!==doc.revision)throw new Error("Camera file changed; reload and reconcile before applying.");
    const verdict=await bridge.checkPzCameraDocument(disk.text);
    if(!verdict.ok)throw new Error(verdict.error);
    // The re-entry of Run discards the frames of a stopped Run: ask first, and tell the server the operator agreed (it refuses otherwise).
    const question=discardQuestion(pzRef.current?.status,pzRef.current?.ringFrames??0);
    if(question&&!await confirm(question))return;
    const result=await bridge.applyPzCameraDocument(doc.path,!!question);
    if(!result.ok)throw new Error(result.message);
    setMessage(result.message);ctx.append(result.message);await ctx.refresh();return;
   }
   if(action==="apply"){
    if(!doc?.path || dirty)throw new Error("Save the draft before applying camera settings.");
    const ctx=current.current;
    if(!ctx.ready||ctx.running||ctx.experimentActive)throw new Error("Stop capture and finalize the experiment before applying camera settings.");
    const selected=await bridge.fetchCameraSelection();
    const expected=kind==="js"?CAMERA_SELECTION_MODES.Hardware:CAMERA_SELECTION_MODES.MindVision;
    if(!selected.valid||!selected.configured||selected.running||selected.mode!==expected)throw new Error(`Select a stopped ${kind==="js"?"EGrabber":"MindVision"} camera.`);
    if(selected.mode!==ctx.selection?.mode || selected.interface_index!==ctx.selection.interface_index || selected.device_index!==ctx.selection.device_index || selected.mindvision_index!==ctx.selection.mindvision_index)throw new Error("Camera selection changed; refresh and retry.");
    const disk=await invoke<Document>("camera_document",{action:"read",path:doc.path,kind,baseline:"",text:""});
    if(disk.revision!==doc.revision)throw new Error("Camera file changed; reload and reconcile before applying.");
    const result=kind==="js"?await bridge.applyCameraScript(doc.path):await bridge.selectMindVisionCamera(selected.mindvision_index,selected.label,doc.path);
    if(!result.ok)throw new Error(result.message);
    setMessage(result.message);ctx.append(result.message);await ctx.refresh();return;
   }
   const path=action==="browse"?await open({multiple:false,filters:[{name:kind==="js"?"EGrabber script":kind==="pz7035"?"PZ7035 camera document":"MindVision configuration",extensions:[kind==="pz7035"?"json":kind]}]}):doc?.path;
   if(typeof path!=="string")return;
   const loaded=await invoke<Document>("camera_document",{action:action==="save"?"save":"read",path,kind,baseline:doc?.revision??"",text:action==="save"?text:""});
   setDoc(loaded);setText(loaded.text);setDirty(false);setMessage(action==="save"?"Saved to disk; not applied to camera.":"Loaded from disk; not applied to camera.");
  }catch(e){setMessage(String(e));current.current.append(`Camera document: ${e}`);}finally{pending.current=false;setBusy(false);}
 };
 return {doc,text,dirty,busy,message,kind,check,run,triggerAllowed:serviceArmed&&context.ready&&context.running&&!context.experimentActive&&context.selection?.mode===CAMERA_SELECTION_MODES.MindVision,edit:(value:string)=>{setText(value);setDirty(true);},applyBlocked:busy||!doc?.path||dirty||!context.ready||context.experimentActive||(kind==="pz7035"?check?.ok!==true:context.running)};
}
function PzDocumentVerdict({check}:{check:PzCameraCheck|null}) {
 if(!check)return <p role="status">Checking the document…</p>;
 if(!check.ok)return <p role="alert" data-testid="pz-document-error">{check.error}</p>;
 return <>
  <p className="mono" data-testid="pz-document-timing">{rateSummary(check)}</p>
  <p className="mono muted" data-testid="pz-document-strobe">{strobeSummary(check)}</p>
  {check.strobe_warning&&<p role="alert">{check.strobe_warning}</p>}
  {!check.qualified&&<p data-testid="pz-document-qualification">{QUALIFIED_NOTE}</p>}
 </>;
}
export function CameraDocumentEditor({model:m}:{model:ReturnType<typeof useCameraDocument>}) {
 const label=m.kind==="js"?"EGrabber script editor":m.kind==="pz7035"?"PZ7035 camera document editor":"MindVision JSON editor";
 const title=m.kind==="js"?"EGrabber camera script":m.kind==="pz7035"?"PZ7035 camera document":"MindVision camera JSON";
 const contents=m.kind==="js"?"Camera script contents":m.kind==="pz7035"?"PZ7035 camera document contents":"MindVision JSON contents";
 return <section aria-label={label}><h5>{title}</h5>
 <div className="toolbar"><button disabled={m.busy} onClick={()=>void m.run("default")}>Load Bundled Default</button><button disabled={m.busy} onClick={()=>void m.run("browse")}>Browse {m.kind==="pz7035"?"JSON":m.kind.toUpperCase()}…</button><button disabled={m.busy||!m.doc?.path} onClick={()=>void m.run("reload")}>Reset / Reload File</button><button disabled={m.busy||!m.doc?.path||!m.dirty} onClick={()=>void m.run("save")}>Save File</button><button disabled={m.busy||!m.doc} onClick={()=>void m.run("saveAs")}>Save As…</button><button disabled={m.applyBlocked} onClick={()=>void m.run("apply")}>Apply Saved File to {m.kind==="pz7035"?"Instrument":"Camera"}</button><button disabled={m.busy||!m.doc} onClick={()=>void m.run("clear")}>Clear File Selection</button></div>
 {m.kind==="json"&&<button disabled={m.busy||!m.triggerAllowed} onClick={()=>void m.run("trigger")} title="Requires armed Service mode, running MindVision camera and idle experiment">MindVision Soft Trigger</button>}
 {m.kind==="pz7035"&&<p>Run frame rate 1,000 to 5,000 fps (whole number) and exposure: "auto" (SHS 64: 150 µs at 5,000 fps, about 950 µs at 1,000 fps) or a number of microseconds that starts the exposure inside the LED strobe window. The LED strobe itself is Service-only and is not part of this document.</p>}
 <p className="mono">{m.doc?.path??"Choose an existing camera file"}</p>{m.doc&&<textarea className="script-editor" aria-label={contents} disabled={m.busy} value={m.text} onChange={e=>m.edit(e.target.value)}/>}
 {m.kind==="pz7035"&&m.doc&&<PzDocumentVerdict check={m.check}/>}
 <p role="status">{m.busy?"Camera document operation pending…":m.message||(m.dirty?"Unsaved edits":"")}</p><p>Browsing, editing and saving never actuate hardware. Apply is explicit{m.kind==="pz7035"?", checks the saved file revision and the document, and re-enters Run when the instrument is in Run.":", requires a stopped matching camera, and verifies the saved file revision."}</p></section>;
}
