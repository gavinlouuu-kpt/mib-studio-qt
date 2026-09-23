import {useRef,useState} from "react";
import {invoke} from "@tauri-apps/api/core";
import {open} from "@tauri-apps/plugin-dialog";
import {bridge} from "./bridge";
import {CAMERA_SELECTION_MODES} from "./bridgeContract";
import type {CameraScriptContext} from "./cameraScript";
type Document={path:string;text:string;revision:string};
export function useCameraDocument(context:CameraScriptContext,kind:"js"|"json") {
 const [doc,setDoc]=useState<Document|null>(null),[text,setText]=useState(""),[dirty,setDirty]=useState(false),[busy,setBusy]=useState(false),[message,setMessage]=useState("");
 const pending=useRef(false),current=useRef(context);current.current=context;
 const run=async(action:"browse"|"reload"|"save"|"apply"|"clear")=>{
  if(pending.current)return;
  if(["browse","reload","clear"].includes(action)&&dirty&&!window.confirm("Discard unsaved camera document edits?"))return;
  pending.current=true;setBusy(true);setMessage("");
  try {
   if(action==="clear"){setDoc(null);setText("");setDirty(false);return;}
   if(action==="apply"){
    if(!doc || dirty)throw new Error("Save the draft before applying camera settings.");
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
   const path=action==="browse"?await open({multiple:false,filters:[{name:kind==="js"?"EGrabber script":"MindVision configuration",extensions:[kind]}]}):doc?.path;
   if(typeof path!=="string")return;
   const loaded=await invoke<Document>("camera_document",{action:action==="save"?"save":"read",path,kind,baseline:doc?.revision??"",text:action==="save"?text:""});
   setDoc(loaded);setText(loaded.text);setDirty(false);setMessage(action==="save"?"Saved to disk; not applied to camera.":"Loaded from disk; not applied to camera.");
  }catch(e){setMessage(String(e));current.current.append(`Camera document: ${e}`);}finally{pending.current=false;setBusy(false);}
 };
 return {doc,text,dirty,busy,message,kind,run,edit:(value:string)=>{setText(value);setDirty(true);},applyBlocked:busy||!doc||dirty||!context.ready||context.running||context.experimentActive};
}
export function CameraDocumentEditor({model:m}:{model:ReturnType<typeof useCameraDocument>}) {
 return <section aria-label={m.kind==="js"?"EGrabber script editor":"MindVision JSON editor"}><h5>{m.kind==="js"?"EGrabber camera script":"MindVision camera JSON"}</h5>
 <div className="toolbar"><button disabled={m.busy} onClick={()=>void m.run("browse")}>Browse {m.kind.toUpperCase()}…</button><button disabled={m.busy||!m.doc} onClick={()=>void m.run("reload")}>Reset / Reload File</button><button disabled={m.busy||!m.doc||!m.dirty} onClick={()=>void m.run("save")}>Save File</button><button disabled={m.applyBlocked} onClick={()=>void m.run("apply")}>Apply Saved File to Camera</button><button disabled={m.busy||!m.doc} onClick={()=>void m.run("clear")}>Clear File Selection</button></div>
 <p className="mono">{m.doc?.path??"Choose an existing camera file"}</p>{m.doc&&<textarea className="script-editor" aria-label={m.kind==="js"?"Camera script contents":"MindVision JSON contents"} disabled={m.busy} value={m.text} onChange={e=>m.edit(e.target.value)}/>}
 <p role="status">{m.busy?"Camera document operation pending…":m.message||(m.dirty?"Unsaved edits":"")}</p><p>Browsing, editing and saving never actuate hardware. Apply is explicit, requires a stopped matching camera, and verifies the saved file revision.</p></section>;
}
