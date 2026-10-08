import {invoke} from "./transport";
import {open, confirm} from "./transport/dialogs";
import { useEffect, useRef, useState } from "react";
import { ProfileCatalogPanel, useProfileCatalog } from "./profileCatalog";
import { configDocument } from "./configDocument";
export interface Profile { name:string; path:string; revision:string; document_json?:string; script?:string|null; profile_id?:string;display_fps?:number }
export interface ProfileReply { ok:boolean; error?:string; profiles?:Profile[]; profile?:Profile; destination?:string; warnings?:string[]; applied?:boolean; message?:string; display_fps?:number; profile_id?:string; active_profile?:Profile|null; selection?:Profile|null; restored?:boolean }
export const profileCommand = (base:string, request:object) => invoke<ProfileReply>("profile_command",{base,request:JSON.stringify(request)});
function draftError(document:string):string {
  if(!document.trim())return "Profile draft is empty. Start a new draft from the current config or import a config.";
  try {
    const parsed=JSON.parse(document);
    if(!parsed||typeof parsed!=="object"||Array.isArray(parsed))return "Profile JSON must be an object";
    if(!Object.keys(parsed).length)return "Profile draft is empty. Start a new draft from the current config or import a config.";
  }catch{return "Profile draft must contain valid JSON";}
  return "";
}
const noCurrentConfig = "Open the app config.json in the App config editor or use Import Config to Draft…";
export function useProfiles({ready,active,resume=false,currentConfig,append,onOpen,onApplied}:{currentConfig:string|null;ready:boolean;active:boolean;resume?:boolean;append:(s:string)=>void;onOpen:(path:string)=>Promise<void>;onApplied?:()=>Promise<void>}) {
  const [base,setBase]=useState(()=>{try{return window.localStorage?.getItem("mib.profiles.directory")??"";}catch{return "";}});
  const [profiles,setProfiles]=useState<Profile[]>([]), [selected,setSelected]=useState<Profile|null>(null);
  const [name,setName]=useState(""),[document,setDocument]=useState(""),[script,setScript]=useState<string|null>(null);
  const [dirty,setDirty]=useState(false),[busy,setBusy]=useState(false),[message,setMessage]=useState("");
  const pending=useRef(false),restored=useRef(false);
  const [activeProfile,setActiveProfile]=useState<Profile|null>(null);
  const run=async(operation:string, target?:Profile)=>{
    if(!ready||active||pending.current)return;
    if(["new","read","import","duplicate"].includes(operation)&&dirty&&!await confirm("Discard unsaved profile draft?"))return;
    if(dirty&&["rename","archive","apply","open"].includes(operation)){setMessage("Save or discard the draft before using saved profile operations.");return;}
    if(operation==="archive"&&!await confirm("Archive this profile? Files remain recoverable in the profile directory."))return;
    pending.current=true;setBusy(true);setMessage("");
    try {
      let directory=base;
      if(operation==="choose") {const chosen=await open({directory:true,multiple:false});if(typeof chosen!=="string")return;directory=chosen;setBase(chosen);try{window.localStorage?.setItem("mib.profiles.directory",chosen);}catch{/* Storage-disabled webviews still support this session. */}setSelected(null);}
      if(operation==="new") {
        if(currentConfig===null)throw new Error(noCurrentConfig);
        const error=draftError(currentConfig);if(error)throw new Error(error);
        setDocument(currentConfig);setScript(null);setSelected(null);setDirty(true);setName("");return;
      }
      if(operation==="import") {const path=await open({multiple:false,filters:[{name:"Application configuration",extensions:["json"]}]});if(typeof path!=="string")return;const d=await configDocument.read(path);if(!d.ok)throw new Error(d.error);setDocument(d.document_json);setScript(null);setDirty(true);return;}
      if(operation==="open") {if(selected)await onOpen(selected.path);return;}
      let q:object={operation:operation==="restore"?"restore":"list"};
      if(operation==="read"&&target)q={operation,name:target.name};
      if(operation==="create") {const error=draftError(document);if(error)throw new Error(error);q={operation,name,document_json:document,script};}
      if(["rename","archive","duplicate","apply"].includes(operation)) {if(!selected)return;q={operation,name:operation==="duplicate"?name:selected.name,source:selected.name,destination:name,baseline:selected.revision};}
      const r=await profileCommand(directory,q);
      if(!r.ok)throw new Error(`${r.applied?"Settings applied; ":""}${r.error}`);
      if(r.applied&&onApplied){try{await onApplied();}catch(e){throw new Error(`Profile applied, but UI refresh failed: ${String(e)}`);}}
      if(r.profile&&operation!=="choose"){setSelected(r.profile);setDocument(r.profile.document_json??"{}");setScript(r.profile.script??null);setDirty(false);if(operation!=="duplicate")setName(r.profile.name);}
      if(operation==="archive"){setSelected(null);setDirty(false);}
      if(r.profiles)setProfiles(r.profiles);
      else {const list=await profileCommand(directory,{operation:"list"});if(!list.ok)throw new Error(list.error);setProfiles(list.profiles??[]);}
      const selection=await profileCommand(directory,{operation:"selection"});
      if(selection.ok)setActiveProfile(selection.active_profile??null);
      const text=r.message??(r.warnings?.length?r.warnings.join("; "):operation==="archive"?`Archived at ${r.destination}`:operation==="create"?"Profile saved. Runtime settings have not changed.":"");
      setMessage(text);if(text)append(text);
    }catch(e){setMessage(String(e));append(`Profiles: ${String(e)}`);}finally{pending.current=false;setBusy(false);}
  };
  useEffect(()=>{if(!ready){restored.current=false;setActiveProfile(null);return;}if(base&&!restored.current){restored.current=true;if(active||resume){void profileCommand(base,{operation:"selection"}).then(r=>{if(r.ok)setActiveProfile(r.active_profile??null);else setMessage(r.error??"Cannot read runtime profile");}).catch(e=>setMessage(String(e)));}else void run("restore");}},[ready,resume,active]);
  const remote=useProfileCatalog({base,selected,blocked:!ready||active||busy,dirty,onInstalled:(p)=>run("read",p),append});
  return {base,profiles,selected,activeProfile,remote,name,setName,document,edit:(v:string)=>{setDocument(v);setDirty(true);},script,editScript:(v:string|null)=>{setScript(v);setDirty(true);},dirty,busy,blocked:!ready||active||busy||remote.busy,message,newDraftError:currentConfig===null?noCurrentConfig:"",saveError:draftError(document),run};
}
// `egrabberScript` false on the PZ7035 (#550 m11): there is no EGrabber camera script there.
export function ProfilesPanel({model:m,egrabberScript=true}:{model:ReturnType<typeof useProfiles>;egrabberScript?:boolean}) {
  return <section aria-label="Local profiles"><h5>Local configuration profiles</h5>
    <p>Choose an existing profiles folder (including one from MIB Studio Qt) or a new local folder. Saving creates a new profile; existing profiles are never silently overwritten.</p>
    <div className="toolbar"><button disabled={m.blocked} onClick={()=>void m.run("choose")}>Choose Profiles Folder…</button><button disabled={m.blocked||!m.base} onClick={()=>void m.run("list")}>Refresh Profiles</button><button disabled={m.blocked||!!m.newDraftError} title={m.newDraftError||undefined} onClick={()=>void m.run("new")}>New Draft from Current Config</button><button disabled={m.blocked} onClick={()=>void m.run("import")}>Import Config to Draft…</button></div>
    <p className="mono">{m.base||"No profile folder selected"}</p><p>Runtime profile: {m.activeProfile?`${m.activeProfile.profile_id??m.activeProfile.name} · ${m.activeProfile.revision.slice(0,12)}`:"none applied"}</p>
    <select aria-label="Profile" disabled={m.blocked||!m.base} value={m.selected?.name??""} onChange={e=>{const p=m.profiles.find(p=>p.name===e.target.value);if(p)void m.run("read",p);}}><option value="">Select profile…</option>{m.profiles.map(p=><option key={p.name} value={p.name}>{p.name}</option>)}</select>
    <label>New profile name<input aria-label="New profile name" value={m.name} disabled={m.blocked} onChange={e=>m.setName(e.target.value)}/></label>
    <div className="toolbar"><button disabled={m.blocked||!m.base||!m.name||!!m.saveError} title={m.saveError||undefined} onClick={()=>void m.run("create")}>Save Draft as New Profile</button><button disabled={m.blocked||!m.selected||!m.name} onClick={()=>void m.run("duplicate")}>Duplicate Saved Profile</button><button disabled={m.blocked||!m.selected||!m.name||m.dirty} onClick={()=>void m.run("rename")}>Rename Profile</button><button disabled={m.blocked||!m.selected||m.dirty} onClick={()=>void m.run("archive")}>Archive Profile</button><button disabled={m.blocked||!m.selected||m.dirty} onClick={()=>void m.run("apply")}>Apply Saved Profile Settings</button><button disabled={m.blocked||!m.selected||m.dirty} onClick={()=>void m.run("open")}>Open Saved Processing Settings</button></div>
    <textarea className="script-editor" aria-label="Profile configuration JSON" disabled={m.blocked} value={m.document} onChange={e=>m.edit(e.target.value)}/>
    {egrabberScript&&<label><input type="checkbox" disabled={m.blocked} checked={m.script!==null} onChange={e=>m.editScript(e.target.checked?"":null)}/>Include EGrabber camera script</label>}
    {egrabberScript&&m.script!==null&&<textarea className="script-editor" aria-label="Profile camera script" disabled={m.blocked} value={m.script} onChange={e=>m.editScript(e.target.value)}/>}
    <p role="status">{m.busy?"Profile operation in progress…":m.message|| (m.dirty?"Unsaved profile draft":"")}</p>
    <ProfileCatalogPanel model={m.remote}/>
    <p>Apply requires stopped capture/realtime and disabled autofocus. It applies processing, buffer, realtime, calibration, delivery mode, autofocus settings and the ROI (validated now, or on the first captured frame if none was captured yet; Start waits for it). It does not connect hardware or execute the optional camera script. Last successfully applied profile settings are restored on startup only if its saved revision is unchanged. Catalog updates are checksum/compatibility checked and preserve a backup; installation never applies settings automatically.</p>
  </section>;
}
