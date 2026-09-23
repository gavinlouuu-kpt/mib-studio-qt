import {useEffect,useRef,useState} from "react";
import {invoke} from "@tauri-apps/api/core";
import {open,confirm} from "@tauri-apps/plugin-dialog";
import {openUrl} from "@tauri-apps/plugin-opener";
interface Release {token:string;version:string;url:string;artifact_family:string;os:string;arch:string;installer_size_bytes:number}
export function ApplicationUpdateControls({channel,blocked,onBusy}:{channel:string;blocked:boolean;onBusy:(busy:boolean)=>void}){
 const [release,setRelease]=useState<Release|null>(null),[verified,setVerified]=useState(false),[busy,setBusy]=useState(false),[message,setMessage]=useState("");
 const pending=useRef(false),latest=useRef({blocked,channel});latest.current={blocked,channel};
 useEffect(()=>{setRelease(null);setVerified(false);},[channel]);
 async function run(action:"check"|"verify"|"launch"|"clear"){
  if(latest.current.blocked||pending.current)return;
  pending.current=true;setBusy(true);onBusy(true);setMessage("");const selectedChannel=channel;
  try{
   if(action==="clear"){
    if(!await confirm("Close all external installer windows/processes first. Clear this application's staged installer packages? Downloads outside its cache and unrelated files are preserved. You must check and verify again afterward.",{title:"Clear Installer Cache",kind:"warning"}))return;
    if(latest.current.blocked)throw new Error("Finish pending work/save drafts before clearing installer cache");
    setRelease(null);setVerified(false);
    const result=await invoke<{removed:number;failed:string[]}>("clear_tauri_installer_cache",{uiIdle:true,confirmed:true});
    setMessage(`Removed ${result.removed} staged installer(s).${result.failed.length?` Some files could not be removed (close installers and retry): ${result.failed.join("; ")}`:""}`);return;
   }
   if(action==="check"){
    setRelease(null);setVerified(false);const result=await invoke<Release>("check_tauri_app_update",{channel});
    if(selectedChannel!==latest.current.channel)throw new Error("Channel changed; check the selected channel again");
    setRelease(result);setMessage("Newer Tauri release checked. Download, then select the installer for verification.");return;
   }
   if(!release)throw new Error("Check a Tauri release first");
   if(action==="verify"){
    setVerified(false);const path=await open({title:"Select downloaded Tauri installer",multiple:false,filters:[{name:"Application installer",extensions:release.os==="windows"?["exe","msi"]:["deb","rpm"]}]});
    if(typeof path!=="string")return;
    if(latest.current.blocked||selectedChannel!==latest.current.channel)throw new Error("UI state changed; finish pending work/save drafts before updating");
    await invoke("verify_tauri_app_installer",{token:release.token,path,uiIdle:true});
    setVerified(true);setMessage("Installer copied into private staging and SHA256 verified. Nothing has been launched.");return;
   }
   if(!verified)throw new Error("Verify the downloaded installer first");
   if(!await confirm(`Launch the verified Tauri ${release.version} installer? The native package installer may request administrator approval. This application stays open; save all work before proceeding.`,{title:"Install Tauri update",kind:"warning"}))return;
   if(latest.current.blocked||selectedChannel!==latest.current.channel)throw new Error("UI state changed; finish pending work/save drafts before updating");
   await invoke("launch_tauri_app_installer",{token:release.token,uiIdle:true,confirmed:true});
   setVerified(false);setRelease(null);setMessage("Verified installer launch accepted. Complete the native installer, then close this application when appropriate. Installation completion has not been confirmed.");
  }catch(e){setMessage(String(e));}finally{pending.current=false;setBusy(false);onBusy(false);}
 }
 return <section aria-label="Application installer update"><h5>Application update</h5>
 <p>Only explicitly identified Tauri packages for this platform are accepted. Existing Qt installers are never launched.</p>
 <button disabled={blocked||busy} onClick={()=>void run("check")}>Check Tauri App Releases</button>
 <button disabled={blocked||busy} onClick={()=>void run("clear")}>Clear Installer Cache…</button>
 {release&&<><p>Tauri {release.version} · {release.os}/{release.arch} · {release.installer_size_bytes} bytes</p>
 <button disabled={blocked||busy} onClick={()=>void openUrl(release.url).catch(e=>setMessage(String(e)))}>Download Tauri Installer</button>
 <button disabled={blocked||busy} onClick={()=>void run("verify")}>Verify Downloaded Installer…</button>
 <button disabled={blocked||busy||!verified} onClick={()=>void run("launch")}>Launch Verified Installer…</button></>}
 {blocked&&<p>Stop capture/recording, finish pending operations, and save/discard drafts before updating.</p>}
 <p role="status">{busy?"Checking/verifying installer…":message}</p></section>;
}
