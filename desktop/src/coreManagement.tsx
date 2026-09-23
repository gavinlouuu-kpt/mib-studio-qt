import {invoke} from "@tauri-apps/api/core";
import {open} from "@tauri-apps/plugin-dialog";
import {openUrl} from "@tauri-apps/plugin-opener";
import {useEffect,useRef,useState} from "react";
import {fetchProfileText} from "./profileCatalog";
interface Native {filename:string;os:string;arch:string;url:string;sha256:string;size_bytes:number;[key:string]:unknown}
interface Entry {version:string;release_tag:string;contract_version:number;manifest_url:string;native_plugins:Native[];[key:string]:unknown}
interface Index {channel:string;processing_core_index_schema_version:number;versions:Entry[]}
interface Info {ok:boolean;error?:string;os:string;arch:string;app_version:string;active_version:string;bundled_version:string;required_version:string;pin_satisfied:boolean;runtime_fingerprint:string}
interface CoreReply {ok:boolean;error?:string;active_version?:string;restored?:boolean}
export const coreCommand=(request:object)=>invoke<CoreReply>("processing_core_command",{request:JSON.stringify(request)});
export function canonicalJson(value:unknown):string {
 const normalized=(v:unknown):unknown=>Array.isArray(v)?v.map(normalized):v&&typeof v==="object"?Object.fromEntries(Object.entries(v).sort(([a],[b])=>a.localeCompare(b)).map(([k,x])=>[k,normalized(x)])):v;
 return JSON.stringify(normalized(value));
}
export function matchingNative(entry:Entry,info:Pick<Info,"os"|"arch">){return entry.native_plugins.find(p=>p.os===info.os&&({amd64:"x86_64",x64:"x86_64",arm64:"aarch64"}[p.arch]??p.arch)===info.arch);}
export function useCoreManagement({ready,active,append,onChanged}:{ready:boolean;active:boolean;append:(s:string)=>void;onChanged:()=>Promise<void>}){
 const [info,setInfo]=useState<Info|null>(null),[initialized,setInitialized]=useState(false),[busy,setBusy]=useState(false),[message,setMessage]=useState("");
 const [channel,setChannel]=useState("stable"),[index,setIndex]=useState<Index|null>(null),[selected,setSelected]=useState(""),[registry,setRegistry]=useState("https://updates.yofo.bio");
 const [appRelease,setAppRelease]=useState<{version:string;url:string;sha256:string;artifact_family?:string}|null>(null);
 const pending=useRef(false),restored=useRef(false);
 const refresh=async()=>{const r=await invoke<Info>("processing_core_command",{request:JSON.stringify({operation:"info"})});if(!r.ok)throw new Error(r.error);setInfo(r);};
 const run=async(action:"restore"|"check"|"activate"|"bundled"|"app")=>{
  if(!ready||active||pending.current)return;pending.current=true;setBusy(true);setMessage("");
  try{
   if(action==="check"){
    const base=registry.replace(/\/+$/,"");if(!base.startsWith("https://"))throw new Error("Core registry requires HTTPS");
    const [history,latest]=await Promise.all([fetchProfileText(`${base}/${channel}/processing-core/index.json`),fetchProfileText(`${base}/${channel}/processing-core/latest.json`)]);
    const i=JSON.parse(history) as Index,l=JSON.parse(latest);
    if(i.processing_core_index_schema_version!==1||i.channel!==channel||!Array.isArray(i.versions)||i.versions.length>1000||l.processing_core_manifest_schema_version!==2||l.channel!==channel)throw new Error("Unsupported core registry schema or channel");
    const canonical=i.versions.find(v=>v.version===l.version);
    if(!canonical||canonical.contract_version!==l.contract_version||canonical.release_tag!==l.wheel?.release_tag||canonicalJson(canonical.native_plugins)!==canonicalJson(l.native_plugins))throw new Error("Canonical latest manifest and registry history disagree");
    setIndex(i);setSelected(l.version);setMessage(`Canonical ${channel} core: ${l.version}`);return;
   }
   if(action==="app"){
    const release=await invoke<{version:string;url:string;sha256:string;artifact_family?:string}>("inspect_app_update",{url:`https://updates.yofo.bio/${channel}/latest.json`});setAppRelease(release);return;
   }
   let request:object={operation:action};
   if(action==="activate"){
    const entry=index?.versions.find(v=>v.version===selected);if(!entry||!info)throw new Error("Select a core version");const plugin=matchingNative(entry,info);if(!plugin)throw new Error("No artifact for this platform");
    if(!entry.manifest_url.startsWith("https://"))throw new Error("Immutable manifest requires HTTPS");
    const manifest=await fetchProfileText(entry.manifest_url);
    const source=await open({multiple:false,filters:[{name:"Downloaded native processing core",extensions:["so","dll","dylib"]}]});if(typeof source!=="string")return;
    if(!window.confirm(`Verify and activate processing core ${entry.version}? Capture and processing must be stopped. The previous core remains active if verification or persistence fails.`))return;
    request={operation:"activate_local",entry,manifest_json:manifest,channel:index!.channel,source_path:source};
   }else if(action==="bundled"&&!window.confirm("Switch to the bundled processing core? Administrator version pins still apply."))return;
   const r=await coreCommand(request);if(!r.ok)throw new Error(r.error);await refresh();await onChanged();setMessage(r.active_version?`Processing core ${r.active_version} active`:"Core startup selection checked");
  }catch(e){setMessage(String(e));append(`Core/update: ${String(e)}`);try{await refresh();}catch{/* Retain primary error. */}}
  finally{pending.current=false;setBusy(false);if(action==="restore")setInitialized(true);}
 };
 useEffect(()=>{if(!ready){restored.current=false;setInitialized(false);return;}if(!restored.current){restored.current=true;if(active){setInitialized(true);void refresh().catch(e=>setMessage(String(e)));}else void run("restore");}},[ready,active]);
 return {info,initialized,report:(s:string)=>setMessage(s),busy,message,channel,setChannel:(c:string)=>{setChannel(c);setIndex(null);},registry,setRegistry:(s:string)=>{setRegistry(s);setIndex(null);},index,selected,setSelected,blocked:!ready||active||busy,appRelease,run};
}
export function CoreManagementPanel({model:m}:{model:ReturnType<typeof useCoreManagement>}){
 const entry=m.index?.versions.find(e=>e.version===m.selected),plugin=entry&&m.info?matchingNative(entry,m.info):undefined;
 return <section aria-label="Processing core and application updates"><h5>Processing core and application updates</h5><p>Active core: {m.info?.active_version??"checking…"}; bundled: {m.info?.bundled_version??"—"}; required: {m.info?.required_version||"none"}</p>
 <div className="toolbar"><select aria-label="Update channel" disabled={m.blocked} value={m.channel} onChange={e=>m.setChannel(e.target.value)}><option>stable</option><option>beta</option></select><button disabled={m.blocked} onClick={()=>void m.run("check")}>Check Processing Cores</button><button disabled={m.blocked} onClick={()=>void m.run("bundled")}>Use Bundled Core</button><button disabled={m.blocked} onClick={()=>void m.run("app")}>Check App Releases</button></div>
 <label>Core registry<input value={m.registry} disabled={m.blocked} onChange={e=>m.setRegistry(e.target.value)}/></label>
 {m.index&&<><select aria-label="Core version" value={m.selected} disabled={m.blocked} onChange={e=>m.setSelected(e.target.value)}>{m.index.versions.map(e=><option key={e.version}>{e.version}</option>)}</select><button disabled={m.blocked||!plugin||!plugin.url.startsWith("https://")} onClick={()=>void openUrl(plugin!.url).catch(e=>m.report(`Download link failed: ${String(e)}`))}>Download Selected Core</button><button disabled={m.blocked||!plugin} onClick={()=>void m.run("activate")}>Verify Downloaded Core and Activate…</button><p>Download the selected native library, then select it for verification. The shared loader checks digest, compiled signer pin, ABI, runtime fingerprint and application bounds; unsigned or mismatched code is never loaded.</p></>}
 {m.appRelease&&<p>Published app release: {m.appRelease.version}. Artifact family: {m.appRelease.artifact_family??"not identified as Tauri"}. Installer launch is unavailable until the release feed explicitly publishes a Tauri package. Existing Qt installers are not installed as Tauri updates.</p>}
 <p role="status">{m.busy?"Checking/verifying core or update…":m.message}</p></section>;
}
