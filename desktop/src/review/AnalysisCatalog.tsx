import {useState} from "react";
import type {AnalysisCall} from "./AnalysisWorkspace";
/** Metadata discovery only; evaluation still requires an exact revision and source. */
export function AnalysisCatalog({read,disabled,kind="library",onOpen}:{read:AnalysisCall;disabled:boolean;kind?:string;onOpen?:(id:string,revision:string)=>Promise<void>}){
 const [entries,setEntries]=useState<Array<{id:string;revision:string}>>([]),[next,setNext]=useState<string|null>(null);
 const [busy,setBusy]=useState(false),[error,setError]=useState("");
 async function run(operation:()=>Promise<void>){setBusy(true);setError("");try{await operation();}catch(e){setError(String(e));}finally{setBusy(false);}}
 async function page(after:string){const r=await read({action:"catalog",kind,after,limit:25});if(r.status!=="ok")throw new Error(r.message||r.status);setEntries(r.entries);setNext(r.next);}
 return <section aria-label={`${kind} catalog`}><fieldset disabled={disabled||busy}>
  <button onClick={()=>void run(()=>page(""))}>Discover saved {kind} records</button>
  <ul>{entries.map(entry=><li key={entry.id}><code>{entry.id}</code> · exact revision {entry.revision}{onOpen&&<button onClick={()=>void run(()=>onOpen(entry.id,entry.revision))}>Open {entry.id} revision {entry.revision}</button>}</li>)}</ul>
  <button disabled={!next} onClick={()=>void run(()=>page(next!))}>Next discovery page</button>
 </fieldset><p>Discovery shows current heads, not a frozen inventory. Opening pins the displayed exact revision; later saves cannot silently move it.</p>{error&&<p role="alert">{error}</p>}</section>;
}
