import {AnalysisReportBatch} from "./AnalysisReportBatch";
import { useRef, useState } from "react";
import type { AnalysisCall } from "./AnalysisWorkspace";
import { AnalysisPlot } from "./AnalysisPlot";
const journal="mib.analysis.pending-report.v1";
/** Scientific data are frozen by the native report builder; this component only lays them out. */
export function AnalysisReport({read,write,disabled}:{read:AnalysisCall;write:AnalysisCall;disabled:boolean}) {
  const [id,setId]=useState("");const [revision,setRevision]=useState("");
  const [runId,setRunId]=useState("");const [busy,setBusy]=useState(false);
  const [message,setMessage]=useState("");const [document,setDocument]=useState<Record<string,any>|null>(null);
  const [layout,setLayout]=useState(JSON.stringify({title:"Research evidence",blocks:[]},null,2));
  const article=useRef<HTMLElement>(null);
  const checked=(reply:Record<string,any>)=>{if(reply.status!=="ok")throw new Error(`${reply.status}: ${reply.message||"Report refused"}`);return reply;};
  async function run(work:()=>Promise<void>){setBusy(true);setMessage("");try{await work();}catch(e){setMessage(String(e));}finally{setBusy(false);}}
  async function submit(command:Record<string,unknown>){
    const reply=checked(await write(command));
    if(window.localStorage.getItem(journal)===JSON.stringify(command))window.localStorage.removeItem(journal);
    if(command.action==="reportSave"){setRevision(reply.revision);setMessage(`Report layout revision ${reply.revision} saved.`);}
    else {setDocument(reply.document);setMessage(`Immutable report run ${command.runId} saved.`);}
  }
  async function mutate(command:Record<string,unknown>){
    if(window.localStorage.getItem(journal))throw new Error("Reconcile the pending report command first.");
    window.localStorage.setItem(journal,JSON.stringify(command));await submit(command);
  }
  const download=(name:string,text:string,type:string)=>{const url=URL.createObjectURL(new Blob([text],{type}));const a=window.document.createElement("a");a.href=url;a.download=name;a.click();URL.revokeObjectURL(url);};
  return <details><summary>Saved report layouts and immutable runs</summary>
    <p>Compose 1–8 table, plot, overlay or finding blocks, each with exact references. Tables and plots require retained snapshots. A build freezes every block in one immutable run; later template edits do not change it. Repeated run names cannot overwrite evidence.</p>
    <fieldset disabled={disabled||busy}>
      <label>Report ID<input value={id} onChange={e=>{setId(e.target.value);setRevision("");}}/></label>
      <label>Exact layout revision<input value={revision} onChange={e=>setRevision(e.target.value)}/></label>
      <label>Report layout JSON<textarea aria-label="Report layout JSON" rows={12} value={layout} onChange={e=>setLayout(e.target.value)}/></label>
      <p>Block: <code>{'{"id":"table-1","title":"Population table","kind":"table","reference":{"planId":"plan","revision":"1","source":{"kind":"retained","id":"snapshot"}}}'}</code>. Plot uses the same reference; overlay uses an exact overlayId/revision; finding uses <code>{'{"findingId":"finding","revision":"1"}'}</code>.</p>
      <button disabled={!id} onClick={()=>void run(()=>mutate({action:"reportSave",reportId:id,expectedRevision:revision||null,commandId:crypto.randomUUID(),layout:JSON.parse(layout)}))}>Save report layout</button>
      <button disabled={!id||!revision} onClick={()=>void run(async()=>{const reply=checked(await read({action:"reportRead",reportId:id,revision}));setLayout(JSON.stringify(reply.document.layout,null,2));})}>Load exact report layout</button>
      <label>Immutable run ID<input value={runId} onChange={e=>setRunId(e.target.value)}/></label>
      <button disabled={!id||!revision||!runId} onClick={()=>void run(()=>mutate({action:"reportBuild",reportId:id,revision,runId,commandId:crypto.randomUUID()}))}>Build immutable report run</button>
      <button disabled={!runId} onClick={()=>void run(async()=>{const reply=checked(await read({action:"reportRunRead",reportId:runId,revision:"1"}));setDocument(reply.document);})}>Reopen report run</button>
      <button onClick={()=>void run(async()=>{const pending=window.localStorage.getItem(journal);if(!pending)throw new Error("No pending report command");await submit(JSON.parse(pending));})}>Reconcile pending report</button>
    </fieldset>
    <AnalysisReportBatch read={read} write={write} disabled={disabled||busy}/>
    <p role="status">{message}</p>
    {document && <>
      <article ref={article} aria-label="Immutable analysis report">
        <h2>{document.artifact.title}</h2>
        <p>Report {document.artifact.reportId} · layout revision {document.artifact.revision} · run {document.request.runId}</p>
        <p>Artifact hash: {document.contentHash}. Descriptive evidence; compatibility and scientific qualification are not inferred.</p>
        {document.artifact.blocks.map((block:Record<string,any>)=><section key={block.spec.id}>
          <h3>{block.spec.title}</h3><p>Exact reference: {JSON.stringify(block.spec.reference)}</p>
          {block.spec.kind==="table" ? <table><thead><tr>{(block.result.report.columns || ["population","selected","unknown","mean","median","unit"]).map((c:string)=><th key={c}>{c}</th>)}</tr></thead>
            <tbody>{block.result.report.table.map((row:Record<string,any>)=><tr key={row.population}>{(block.result.report.columns || ["population","selected","unknown","mean","median","unit"]).map((k:string)=><td key={k}>{row[k]??"—"}</td>)}</tr>)}</tbody></table>
            :(block.spec.kind==="plot" || block.spec.kind==="overlay") ? <AnalysisPlot artifact={block.result.plot}/> : <pre>{JSON.stringify(block.result.document,null,2)}</pre>}
        </section>)}
      </article>
      <button onClick={()=>download("analysis-report.json",JSON.stringify(document,null,2),"application/json")}>Download immutable report JSON</button>
      <button onClick={()=>{if(!article.current)return;const clone=article.current.cloneNode(true) as HTMLElement;clone.querySelectorAll("button").forEach(b=>b.remove());download("analysis-report.html",'<!doctype html><html lang="en"><meta charset="utf-8"><title>Analysis report</title><style>body{font:16px system-ui;margin:2rem;max-width:1100px}td,th{padding:.5rem;border:1px solid #bbb}table{border-collapse:collapse}pre,p{overflow-wrap:anywhere;white-space:pre-wrap}section{break-inside:avoid}</style><body>'+clone.outerHTML+'</body></html>',"text/html");}}>Download standalone report HTML</button>
    </>}
  </details>;
}
