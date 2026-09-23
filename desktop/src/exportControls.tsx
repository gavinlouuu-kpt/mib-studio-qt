import { useEffect, useRef, useState } from "react";
import { open, save } from "@tauri-apps/plugin-dialog";
import { bridge } from "./bridge";
import type { ReviewExportRequest, ReviewExportStatus } from "./reviewExport";

// App owns the hook: navigation cannot abandon status reconciliation/cancellation.
export function useReviewExport(ready: boolean, append: (s:string)=>void) {
  const [frames,setFrames]=useState<"valid"|"invalid"|"both">("both");
  const [seriesEnabled,setSeriesEnabled]=useState(true),[seriesStart,setSeriesStart]=useState("0"),[seriesEnd,setSeriesEnd]=useState("");
  const [isoelastic,setIsoelastic]=useState(true);
  const [status, setStatus] = useState<ReviewExportStatus>({state:"idle"});
  const [pending, setPending] = useState(false);
  const [reconciled,setReconciled]=useState(false);
  const [error, setError] = useState("");
  const [statusError, setStatusError] = useState("");
  const submitting = useRef(false);
  const polling = useRef(false);
  const generation = useRef(0);
  const currentStatus = useRef(status);
  currentStatus.current = status;
  async function refresh() {
    if (polling.current) return;
    const gen = generation.current;
    polling.current = true;
    try {
      const next = await bridge.reviewExportStatus();
      if (gen === generation.current) { setStatus(next); setReconciled(true); setStatusError(""); }
    } catch (e) { if (gen === generation.current) setStatusError(`Export status unavailable: ${String(e)}`); }
    finally { polling.current = false; }
  }
  useEffect(() => {
    ++generation.current;
    if (!ready) {setReconciled(false);return;}
    let stopped = false;
    let timer: ReturnType<typeof setTimeout>;
    const poll = async () => {
      await refresh();
      if (!stopped) timer = setTimeout(() => void poll(), 500);
    };
    void poll();
    return () => { stopped=true; ++generation.current; clearTimeout(timer); };
  }, [ready]);
  async function start(format: ReviewExportRequest["format"], conversionFactor?: number, batch = false) {
    if (!ready || !reconciled || submitting.current || currentStatus.current.state === "running") return;
    submitting.current=true; setPending(true); setError("");
    try {
      const start=Number(seriesStart),end=seriesEnd.trim()===""?undefined:Number(seriesEnd);
      if(!Number.isSafeInteger(start) || start<0 || (end!==undefined && (!Number.isSafeInteger(end) || end<start)))throw new Error("Series range requires nonnegative integer start and end ≥ start.");
      const sources = batch ? await open({title:"Choose HDF files to export",multiple:true,filters:[{name:"HDF5",extensions:["h5","hdf5"]}]}) : undefined;
      if (batch && (!Array.isArray(sources) || !sources.length)) return;
      const picked = format === "metrics_csv" && !batch
        ? await save({title:"Export metrics CSV",filters:[{name:"CSV",extensions:["csv"]}],defaultPath:"metrics.csv"})
        : await open({title:"Choose export parent folder",directory:true,multiple:false});
      if (typeof picked !== "string") return;
      const request: ReviewExportRequest = {
        output_root: format === "metrics_csv" && !batch ? picked.replace(/[/\\][^/\\]*$/, "") : picked,
        format, frames, series:{enabled:seriesEnabled,start,...(end===undefined?{}:{end})},isoelastic_overlays:isoelastic, ...(conversionFactor === undefined ? {} : {conversion_factor:conversionFactor}), keep_partial_on_failure:true,
        ...(format === "metrics_csv" && !batch ? {explicit_destination:picked} : {}),
        ...(Array.isArray(sources) ? {source_paths:sources} : {}),
      };
      const result = await bridge.reviewExport(request);
      if (!result.ok) throw new Error(result.message);
      ++generation.current;
      const accepted: ReviewExportStatus = {state:"running",operation_id:result.operation_id};
      currentStatus.current=accepted;setStatus(accepted);
      append(`Export accepted (operation ${result.operation_id}); awaiting result.`);
      await refresh();
    } catch (e) { const message=String(e); setError(message); append(`Export: ${message}`); }
    finally { submitting.current=false; setPending(false); }
  }
  async function cancel() {
    const current = currentStatus.current;
    if (!ready || submitting.current || current.state !== "running" || !current.operation_id) return;
    submitting.current=true; setPending(true);
    try {
      const result=await bridge.cancelOperation(current.operation_id);
      append(result.ok ? "Export cancellation requested; awaiting terminal result." : `Export cancel: ${result.message}`);
      if (!result.ok) setError(result.message);
      await refresh();
    } catch (e) { setError(String(e)); }
    finally { submitting.current=false; setPending(false); }
  }
  return {status,pending,options:{frames,setFrames,seriesEnabled,setSeriesEnabled,seriesStart,setSeriesStart,seriesEnd,setSeriesEnd,isoelastic,setIsoelastic},error:error || statusError,start,cancel,busy:(ready&&!reconciled) || pending || status.state==="running"};
}

export function ExportStatus({model}:{model:ReturnType<typeof useReviewExport>}) {
  if (model.status.state === "idle" && !model.pending && !model.error) return null;
  return <section aria-label="Export status" className="quality-panel">
    <p role="status">Export: {model.pending ? "request pending" : model.status.state}
      {model.status.file_count && model.status.file_count > 1 ? ` · file ${model.status.file_index ?? model.status.results?.length ?? 0}/${model.status.file_count}` : ""}
      {model.status.phase ? ` · ${model.status.phase}` : ""}
      {model.status.completed !== undefined ? ` · ${model.status.completed}/${model.status.total ?? "unknown"}` : ""}
    </p>
    {model.status.state === "running" && <button disabled={model.pending} onClick={()=>void model.cancel()}>Cancel Export</button>}
    {model.status.final_path && <p className="mono">Output: {model.status.final_path}</p>}
    {model.status.retained_partial_path && <p className="mono">Incomplete output retained: {model.status.retained_partial_path}</p>}
    {(model.error || model.status.error) && <p role="alert">{model.error || model.status.error}</p>}
    {model.status.results && model.status.file_count !== 1 && <ul>{model.status.results.map((result,index)=><li key={index}>{result.source_path}: {result.state} {result.final_path || result.retained_partial_path || result.error}</li>)}</ul>}
    {model.status.warnings?.map((warning,index)=><p key={index}>{warning}</p>)}
  </section>;
}

export function ReviewExportOptions({model}:{model:ReturnType<typeof useReviewExport>}) {
  const options=model.options;
  return <details><summary>Export frame selection, series range and reference overlays</summary>
    <label>Frame class <select disabled={model.busy} value={options.frames} onChange={e=>options.setFrames(e.target.value as typeof options.frames)}><option value="both">Valid and invalid</option><option value="valid">Valid only</option><option value="invalid">Invalid only</option></select></label>
    <label><input type="checkbox" checked={options.seriesEnabled} disabled={model.busy} onChange={e=>options.setSeriesEnabled(e.target.checked)}/>Export stored series images</label>
    <label>Series start (zero-based) <input type="number" min="0" step="1" value={options.seriesStart} disabled={model.busy} onChange={e=>options.setSeriesStart(e.target.value)}/></label>
    <label>Series end (inclusive; blank = remaining) <input type="number" min="0" step="1" value={options.seriesEnd} disabled={model.busy} onChange={e=>options.setSeriesEnd(e.target.value)}/></label>
    <label><input type="checkbox" checked={options.isoelastic} disabled={model.busy} onChange={e=>options.setIsoelastic(e.target.checked)}/>Include fixed-reference isoelastic curves in exported charts</label>
  </details>;
}
