import {invoke} from "@tauri-apps/api/core";
import {useEffect,useRef,useState} from "react";
import {profileCommand, type Profile} from "./profiles";
export interface CatalogEntry {profile_id:string;display_name?:string;description?:string;revision:string;config_url:string;config_sha256:string;camera_script_url?:string;camera_script_sha256?:string;app_min_version?:string|null;app_max_version?:string|null;processing_contract_version?:number|null}
interface Catalog {catalog_schema_version:number;channel?:string;profiles:CatalogEntry[]}
interface Candidate {entry:CatalogEntry;config:string;script:string|null;localName?:string;baseline?:string;localScript?:string|null}
export async function fetchProfileText(url:string):Promise<string>{
  const parsed=new URL(url);if(!["http:","https:"].includes(parsed.protocol)||parsed.username||parsed.password)throw new Error("Use an HTTP(S) URL without credentials");
  const result=await invoke<{ok:boolean;body?:string;error?:string}>("profile_fetch_url",{url});
  if(!result.ok||typeof result.body!=="string")throw new Error(result.error??"Profile download failed");return result.body;
}
export interface Difference {path:string;before:string;after:string}
export function profileDifferences(before:unknown,after:unknown,path=""):Difference[]{
  if(JSON.stringify(before)===JSON.stringify(after))return [];
  if(before&&after&&typeof before==="object"&&typeof after==="object"&&!Array.isArray(before)&&!Array.isArray(after)){
    const a=before as Record<string,unknown>,b=after as Record<string,unknown>;
    return [...new Set([...Object.keys(a),...Object.keys(b)])].sort().flatMap(key=>profileDifferences(a[key],b[key],path?`${path}.${key}`:key));
  }
  return [{path:path||"$",before:before===undefined?"(absent)":JSON.stringify(before),after:after===undefined?"(absent)":JSON.stringify(after)}];
}
export function useProfileCatalog({base,selected,blocked,dirty,onInstalled,append}:{base:string;selected:Profile|null;blocked:boolean;dirty:boolean;onInstalled:(p:Profile)=>Promise<void>;append:(s:string)=>void}){
 const [url,setUrl]=useState("https://updates.yofo.bio/profiles/stable/catalog.json");
 const [catalog,setCatalog]=useState<Catalog|null>(null),[remoteId,setRemoteId]=useState("");
 const [candidate,setCandidate]=useState<Candidate|null>(null),[name,setName]=useState("");
 const [busy,setBusy]=useState(false),[message,setMessage]=useState("");const pending=useRef(false);
 useEffect(()=>{setCandidate(null);setName(selected?.name??"");},[selected?.name,selected?.revision,base]);
 const context=useRef("");context.current=`${base}\0${selected?.name}\0${selected?.revision}`;
 const run=async(action:"check"|"preview"|"install")=>{
  if(blocked||pending.current)return;
  if(action==="install"&&dirty){setMessage("Save or discard the local draft first.");return;}
  const startingContext=context.current;pending.current=true;setBusy(true);setMessage("");
  try{
   if(action==="check"){
    const c=JSON.parse(await fetchProfileText(url)) as Catalog;
    if(c.catalog_schema_version!==1||!Array.isArray(c.profiles)||c.profiles.length>1000)throw new Error("Unsupported profile catalog schema");
    for(const e of c.profiles){if(!e.profile_id||!e.revision||!e.config_url||!e.config_sha256)throw new Error("Catalog entry lacks identity, revision, URL or checksum");}
    setCatalog(c);setRemoteId(c.profiles[0]?.profile_id??"");setCandidate(null);setMessage(`${c.profiles.length} catalog profiles. Checking does not change local files or drafts.`);return;
   }
   if(action==="preview"){
    const entry=catalog?.profiles.find(e=>e.profile_id===remoteId);if(!entry)throw new Error("Choose a remote profile");
    const [config,script]=await Promise.all([fetchProfileText(entry.config_url),entry.camera_script_url?fetchProfileText(entry.camera_script_url):Promise.resolve(null)]);
    const parsed=JSON.parse(config);if(!parsed||typeof parsed!=="object"||Array.isArray(parsed))throw new Error("Remote config must be an object");
    if(context.current!==startingContext)throw new Error("Local profile selection changed during download; preview again");
    setCandidate({entry,config,script,localName:selected?.name,baseline:selected?.revision,localScript:selected?.script});setName(selected?.name??entry.profile_id);return;
   }
   if(!candidate||!name||!base)throw new Error("Preview a catalog profile and choose a local name first");
   if(!window.confirm(`Install ${candidate.entry.profile_id} revision ${candidate.entry.revision} as '${name}'? Existing contents are backed up; runtime settings will not change.`))return;
   const r=await profileCommand(base,{operation:"install_remote",name,baseline:name===candidate.localName?candidate.baseline:"",entry:candidate.entry,document_json:candidate.config,script:candidate.script,catalog_url:url,channel:catalog?.channel??"stable"});
   if(!r.ok)throw new Error(r.error);if(r.profile)await onInstalled(r.profile);setCandidate(null);setMessage("Verified remote profile installed. Runtime settings were not changed; apply the saved profile explicitly.");append("Verified remote profile installed; no automatic application.");
  }catch(e){setMessage(String(e));}finally{pending.current=false;setBusy(false);}
 };
 let differences:Difference[]=[];
 if(candidate){try{differences=profileDifferences(JSON.parse(selected?.document_json??"{}"),JSON.parse(candidate.config));}catch{/* Native read/import already reports malformed local documents. */}}
 return {url,setUrl:(value:string)=>{setUrl(value);setCatalog(null);setCandidate(null);},catalog,remoteId,setRemoteId:(id:string)=>{setRemoteId(id);setCandidate(null);},candidate,name,setName,busy,blocked:blocked||busy,dirty,message,differences,run};
}
export function ProfileCatalogPanel({model:m}:{model:ReturnType<typeof useProfileCatalog>}){
 return <details><summary>Profile catalog and updates</summary>
 <label>Catalog URL<input aria-label="Profile catalog URL" value={m.url} disabled={m.blocked} onChange={e=>{m.setUrl(e.target.value);}}/></label><button disabled={m.blocked} onClick={()=>void m.run("check")}>Check Profile Updates</button>
 {m.catalog&&<><select aria-label="Remote profile" value={m.remoteId} disabled={m.blocked} onChange={e=>m.setRemoteId(e.target.value)}>{m.catalog.profiles.map(e=><option key={e.profile_id} value={e.profile_id}>{e.display_name??e.profile_id} · {e.revision}</option>)}</select><button disabled={m.blocked||!m.remoteId} onClick={()=>void m.run("preview")}>Preview Profile Diff</button></>}
 {m.candidate&&<><p>{m.differences.length} configuration changes. Hardware-related settings and camera scripts require particular review.</p><pre aria-label="Profile configuration diff">{m.differences.slice(0,500).map(d=>`${d.path}\n  local: ${d.before}\n  remote: ${d.after}`).join("\n")}{m.differences.length>500?"\nFurther differences omitted; review complete JSON before installation.":""}</pre><details><summary>Camera script {m.candidate.localScript===m.candidate.script?"unchanged":"changed"} (not executed)</summary><h6>Local</h6><pre>{m.candidate.localScript??"No local script"}</pre><h6>Remote</h6><pre>{m.candidate.script??"No script supplied. Updating removes any old profile script; the backup retains it."}</pre></details><label>Install profile name<input aria-label="Install profile name" value={m.name} disabled={m.blocked} onChange={e=>m.setName(e.target.value)}/></label><button disabled={m.blocked||m.dirty||!m.name} onClick={()=>void m.run("install")}>Install Verified Profile</button></>}
 <p role="status">{m.busy?"Profile catalog operation in progress…":m.message}</p></details>;
}
