import { useState } from "react";
import type { AnalysisCall } from "./AnalysisWorkspace";
const journal="mib.analysis.pending-library.v1";
const list=(s:string)=>s.split(",").map(v=>v.trim()).filter(Boolean);
type Sample={source:{kind:string;id:string};label:string;groups:string[];tags:string[];notes:string;preferences:Record<string,any>};
export function AnalysisLibrary({read,write,disabled,onPrecision}:{read:AnalysisCall;write:AnalysisCall;disabled:boolean;onPrecision:(digits:number)=>void}){
 const [id,setId]=useState("");const [revision,setRevision]=useState("");const [title,setTitle]=useState("");const [groups,setGroups]=useState("");const [digits,setDigits]=useState(4);
 const [samples,setSamples]=useState<Sample[]>([]);const [snapshot,setSnapshot]=useState("");const [label,setLabel]=useState("");
 const [busy,setBusy]=useState(false);const [message,setMessage]=useState("");const [saved,setSaved]=useState<Record<string,any>|null>(null);
 const checked=(r:Record<string,any>)=>{if(r.status!=="ok")throw new Error(`${r.status}: ${r.message||"Library refused"}`);return r;};
 async function run(work:()=>Promise<void>){setBusy(true);setMessage("");try{await work();}catch(e){setMessage(String(e));}finally{setBusy(false);}}
 function load(reply:Record<string,any>){const library=reply.document.library;setTitle(library.title);setGroups(library.groups.join(", "));setDigits(library.preferences.decimalDigits??4);setSamples(library.samples.map(({binding,...sample}:Record<string,any>)=>sample as Sample));setRevision(reply.revision);setSaved(reply);}
 async function submit(command:Record<string,unknown>){const reply=checked(await write(command));if(window.localStorage.getItem(journal)===JSON.stringify(command))window.localStorage.removeItem(journal);setRevision(reply.revision);setSaved(null);setMessage(`Library revision ${reply.revision} saved. Open this exact revision for verified identities and preference provenance.`);}
 function edit(index:number,patch:Partial<Sample>){setSamples(old=>old.map((s,i)=>i===index?{...s,...patch}:s));}
 return <details><summary>Saved sample library and groups</summary>
  <p>Retain source snapshots in the workspace first. Labels, groups, tags and notes are annotations; they never change recordings or measurement units. Duplicate source fingerprints are refused even when labels differ.</p>
  <fieldset disabled={disabled||busy}>
   <label>Library ID<input value={id} onChange={e=>{setId(e.target.value);setRevision("");setSaved(null);}}/></label>
   <label>Exact library revision<input value={revision} onChange={e=>setRevision(e.target.value)}/></label>
   <label>Study title<input value={title} onChange={e=>setTitle(e.target.value)}/></label>
   <label>Declared groups (comma separated)<input value={groups} onChange={e=>setGroups(e.target.value)}/></label>
   <label>Library display precision<input type="number" min="2" max="8" value={digits} onChange={e=>setDigits(Number(e.target.value))}/></label>
   <label>Retained snapshot ID<input value={snapshot} onChange={e=>setSnapshot(e.target.value)}/></label>
   <label>Sample label<input value={label} onChange={e=>setLabel(e.target.value)}/></label>
   <button disabled={!snapshot||!label||samples.length>=128} onClick={()=>{setSamples(old=>[...old,{source:{kind:"retained",id:snapshot},label,groups:[],tags:[],notes:"",preferences:{}}]);setSnapshot("");setLabel("");}}>Add retained sample</button>
   {samples.map((s,i)=><fieldset key={i}><legend>Sample {i+1}: {s.source.id}</legend>
    <label>Label<input value={s.label} onChange={e=>edit(i,{label:e.target.value})}/></label>
    <label>Groups<input key={s.source.id+JSON.stringify(s.groups)} defaultValue={s.groups.join(", ")} onBlur={e=>edit(i,{groups:list(e.target.value)})}/></label>
    <label>Tags<input key={s.source.id+JSON.stringify(s.tags)} defaultValue={s.tags.join(", ")} onBlur={e=>edit(i,{tags:list(e.target.value)})}/></label>
    <label>Notes<textarea value={s.notes} onChange={e=>edit(i,{notes:e.target.value})}/></label>
    <label>Sample precision override (blank inherits)<input type="number" min="2" max="8" value={s.preferences.decimalDigits??""} onChange={e=>edit(i,{preferences:e.target.value?{decimalDigits:Number(e.target.value)}:{}})}/></label>
    <button onClick={()=>setSamples(old=>old.filter((_,j)=>j!==i))}>Remove annotation entry</button>
   </fieldset>)}
   <button disabled={!id||!title} onClick={()=>void run(async()=>{if(window.localStorage.getItem(journal))throw new Error("Reconcile pending library first.");const command={action:"librarySave",libraryId:id,expectedRevision:revision||null,commandId:crypto.randomUUID(),library:{title,groups:list(groups),preferences:{decimalDigits:digits},samples}};window.localStorage.setItem(journal,JSON.stringify(command));await submit(command);})}>Save library revision</button>
   <button disabled={!id||!revision} onClick={()=>void run(async()=>load(checked(await read({action:"libraryRead",libraryId:id,revision}))))}>Open exact library</button>
   <button onClick={()=>void run(async()=>{const pending=window.localStorage.getItem(journal);if(!pending)throw new Error("No pending library command");await submit(JSON.parse(pending));})}>Reconcile pending library</button>
  </fieldset>
  <p role="status">{message}</p>
  {saved && <section aria-label="Verified sample library"><h5>{saved.document.library.title} · revision {saved.revision}</h5>
   {saved.document.library.samples.map((s:Record<string,any>,i:number)=><div key={s.binding.fingerprint}><strong>{s.label}</strong><p>{s.groups.join(", ")} · {s.binding.fingerprint}</p><p>Units: {JSON.stringify(s.binding.parameters)}</p><p>Display preferences: {JSON.stringify(saved.display[i].preferences)}</p><button onClick={()=>onPrecision(saved.display[i].preferences.values.decimalDigits)}>Use this sample’s display precision</button></div>)}
  </section>}
 </details>;
}
