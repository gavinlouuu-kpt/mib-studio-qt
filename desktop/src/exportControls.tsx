import { useEffect, useRef, useState } from "react";
import {open, save} from "./transport/dialogs";
import { bridge } from "./bridge";
import type { ReviewExportRequest, ReviewExportStatus } from "./reviewExport";
import { describeExportFile, exportDialogPlan } from "./reviewExport";
import { dirName, exportDir, rememberExportDir } from "./review/exports/exportHelpers";

/** MIB Studio's remembered export folder (the Qt tab's HdfReviewTab/lastExportDir;
 * YOFO Review keeps its own, `yofo.review.lastExportDir`). */
export const STUDIO_LAST_EXPORT_DIR_KEY = "mib.review.lastExportDir";

// App owns the hook: navigation cannot abandon status reconciliation/cancellation.
export function useReviewExport(ready: boolean, append: (s:string)=>void) {
  const [frames,setFrames]=useState<"valid"|"invalid"|"both">("both");
  const [seriesEnabled,setSeriesEnabled]=useState(true),[seriesStart,setSeriesStart]=useState("0"),[seriesEnd,setSeriesEnd]=useState("");
  const [isoelastic,setIsoelastic]=useState(true);
  // Qt discards a failed export's partial output; keeping it is the operator's choice.
  const [keepPartial,setKeepPartial]=useState(false);
  // Folder to remember once the started operation completes (Qt remembers on success only).
  const rememberOnSuccess = useRef<{operation:string;dir:string}|null>(null);
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
      const pendingDir = rememberOnSuccess.current;
      if (pendingDir && next.operation_id === pendingDir.operation && next.state !== "running") {
        rememberOnSuccess.current = null;
        if (next.state === "completed") rememberExportDir(pendingDir.dir, undefined, STUDIO_LAST_EXPORT_DIR_KEY);
      }
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
  /** Start an export. `fallbackFactor` (the live processing factor) is used only for a file
   * that records no px→µm factor: every file is exported at its own recorded factor, as the
   * Qt tab does (TD-17, #854). */
  async function start(format: ReviewExportRequest["format"], fallbackFactor?: number, batch = false) {
    if (!ready || !reconciled || submitting.current || currentStatus.current.state === "running") return;
    submitting.current=true; setPending(true); setError("");
    try {
      const start=Number(seriesStart),end=seriesEnd.trim()===""?undefined:Number(seriesEnd);
      if(!Number.isSafeInteger(start) || start<0 || (end!==undefined && (!Number.isSafeInteger(end) || end<start)))throw new Error("Series range requires nonnegative integer start and end ≥ start.");
      const source = batch ? undefined : await bridge.fetchReviewMetadata();
      if (!batch && (!source?.file_open || !source.file_path)) throw new Error("No review source is open.");
      if (!batch && source?.recording_file && (format === "metrics_csv" || format === "fcs" || format === "charts"))
        throw new Error("Recording files carry no object metrics.");
      const openFile = source?.file_path ?? "";
      const plan = exportDialogPlan(format, batch, openFile, exportDir(openFile, undefined, STUDIO_LAST_EXPORT_DIR_KEY));
      let sources: string[] | undefined;
      if (plan.files) {
        const picked = await open({title:plan.files.title,multiple:true,filters:plan.files.filters,...(plan.files.defaultPath?{defaultPath:plan.files.defaultPath}:{})});
        sources = Array.isArray(picked) ? picked : typeof picked === "string" ? [picked] : [];
        if (!sources.length) return;
      }
      const picked = plan.destination.kind === "file"
        ? await save({title:plan.destination.title,filters:plan.destination.filters,defaultPath:plan.destination.defaultPath})
        : await open({title:plan.destination.title,directory:true,multiple:false,...(plan.destination.defaultPath?{defaultPath:plan.destination.defaultPath}:{})});
      if (typeof picked !== "string" || !picked) return;
      const destination = plan.destination.kind === "file" ? plan.destination.normalize(picked) : picked;
      const root = plan.destination.kind === "file" ? dirName(destination) || "." : destination;
      const request: ReviewExportRequest = {
        output_root: root,
        format, frames, series:{enabled:seriesEnabled,start,...(end===undefined?{}:{end})},isoelastic_overlays:isoelastic,
        conversion_factor_source:"recorded", ...(fallbackFactor === undefined ? {} : {conversion_factor:fallbackFactor}),
        keep_partial_on_failure:keepPartial,
        ...(batch && (format === "metrics_csv" || format === "fcs") ? {refuse_recordings:true} : {}),
        ...(plan.destination.kind === "file" ? {explicit_destination:destination} : {}),
        // A single export names its file, so switching the review file during the dialog
        // cannot change what is exported.
        source_paths: sources ?? [openFile],
      };
      const result = await bridge.reviewExport(request);
      if (!result.ok) throw new Error(result.message);
      ++generation.current;
      rememberOnSuccess.current = {operation:result.operation_id,dir:root};
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
  return {status,pending,options:{frames,setFrames,seriesEnabled,setSeriesEnabled,seriesStart,setSeriesStart,seriesEnd,setSeriesEnd,isoelastic,setIsoelastic,keepPartial,setKeepPartial},error:error || statusError,start,cancel,busy:(ready&&!reconciled) || pending || status.state==="running"};
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
    {model.status.state !== "running" && model.status.results?.length === 1 && describeExportFile(model.status.results[0]) &&
      <p className="mono" data-testid="export-file-detail">{describeExportFile(model.status.results[0])}</p>}
    {model.status.retained_partial_path && <p className="mono">Incomplete output retained: {model.status.retained_partial_path}</p>}
    {(model.error || model.status.error) && <p role="alert">{model.error || model.status.error}</p>}
    {model.status.results && model.status.file_count !== 1 && <ul>{model.status.results.map((result,index)=><li key={index}>{result.source_path}: {result.state} {result.final_path || result.retained_partial_path || result.error}{describeExportFile(result) ? ` · ${describeExportFile(result)}` : ""}</li>)}</ul>}
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
    <label title="The Qt tab discards a failed export's partial output"><input type="checkbox" checked={options.keepPartial} disabled={model.busy} onChange={e=>options.setKeepPartial(e.target.checked)}/>Keep partial output when an export fails (with a failure manifest)</label>
  </details>;
}
