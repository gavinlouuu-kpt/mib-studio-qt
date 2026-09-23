import { useEffect, useRef, useState } from "react";
import { open, save } from "@tauri-apps/plugin-dialog";
import { bridge } from "./bridge";
import type { ReviewExportRequest, ReviewExportStatus } from "./reviewExport";

// App owns the hook: navigation cannot abandon status reconciliation/cancellation.
export function useReviewExport(ready: boolean, append: (s:string)=>void) {
  const [status, setStatus] = useState<ReviewExportStatus>({state:"idle"});
  const [pending, setPending] = useState(false);
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
      if (gen === generation.current) { setStatus(next); setStatusError(""); }
    } catch (e) { if (gen === generation.current) setStatusError(`Export status unavailable: ${String(e)}`); }
    finally { polling.current = false; }
  }
  useEffect(() => {
    ++generation.current;
    if (!ready) return;
    let stopped = false;
    let timer: ReturnType<typeof setTimeout>;
    const poll = async () => {
      await refresh();
      if (!stopped) timer = setTimeout(() => void poll(), 500);
    };
    void poll();
    return () => { stopped=true; ++generation.current; clearTimeout(timer); };
  }, [ready]);
  async function start(format: ReviewExportRequest["format"], conversionFactor?: number) {
    if (!ready || submitting.current || currentStatus.current.state === "running") return;
    submitting.current=true; setPending(true); setError("");
    try {
      const picked = format === "metrics_csv"
        ? await save({title:"Export metrics CSV",filters:[{name:"CSV",extensions:["csv"]}],defaultPath:"metrics.csv"})
        : await open({title:"Choose export parent folder",directory:true,multiple:false});
      if (typeof picked !== "string") return;
      const request: ReviewExportRequest = {
        output_root: format === "metrics_csv" ? picked.replace(/[/\\][^/\\]*$/, "") : picked,
        format, frames:"both", ...(conversionFactor === undefined ? {} : {conversion_factor:conversionFactor}), keep_partial_on_failure:true,
        ...(format === "metrics_csv" ? {explicit_destination:picked} : {}),
      };
      const result = await bridge.reviewExport(request);
      if (!result.ok) throw new Error(result.message);
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
  return {status,pending,error:error || statusError,start,cancel,busy:pending || status.state==="running"};
}

export function ExportStatus({model}:{model:ReturnType<typeof useReviewExport>}) {
  if (model.status.state === "idle" && !model.pending && !model.error) return null;
  return <section aria-label="Export status" className="quality-panel">
    <p role="status">Export: {model.pending ? "request pending" : model.status.state}
      {model.status.phase ? ` · ${model.status.phase}` : ""}
      {model.status.completed !== undefined ? ` · ${model.status.completed}/${model.status.total ?? "unknown"}` : ""}
    </p>
    {model.status.state === "running" && <button disabled={model.pending} onClick={()=>void model.cancel()}>Cancel Export</button>}
    {model.status.final_path && <p className="mono">Output: {model.status.final_path}</p>}
    {model.status.retained_partial_path && <p className="mono">Incomplete output retained: {model.status.retained_partial_path}</p>}
    {(model.error || model.status.error) && <p role="alert">{model.error || model.status.error}</p>}
    {model.status.warnings?.map((warning,index)=><p key={index}>{warning}</p>)}
  </section>;
}
