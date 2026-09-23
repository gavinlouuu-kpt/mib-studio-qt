import { useEffect, useState } from "react";
import { bridge, type ReviewMetricsPage } from "../bridge";
import { MonitoringCharts } from "./MonitoringCharts";

/** Read-only, bounded saved-file charts. Never substitute live monitoring rows. */
export function ReviewCharts({sourcePath}:{sourcePath:string}) {
  const [valid,setValid]=useState(true);
  const [offset,setOffset]=useState(0);
  const [page,setPage]=useState<ReviewMetricsPage|null>(null);
  const [error,setError]=useState("");
  const [loading,setLoading]=useState(false);
  useEffect(()=>{setOffset(0);setPage(null);},[sourcePath]);
  useEffect(()=>{
    let active=true;
    setPage(null);setError("");
    if (!sourcePath) return;
    setLoading(true);
    void (async()=>{
      try {
        const before=await bridge.fetchReviewMetadata();
        if (!before.file_open || before.file_path!==sourcePath) throw new Error("Review source changed. Reload the file before charting.");
        const next=await bridge.fetchReviewMetricsPage(valid,offset,200);
        const after=await bridge.fetchReviewMetadata();
        if (!next.valid || !after.file_open || after.file_path!==sourcePath) throw new Error("Review source changed or metrics could not be read.");
        if (active) setPage(next);
      } catch(e) {if(active)setError(String(e));}
      finally {if(active)setLoading(false);}
    })();
    return ()=>{active=false;};
  },[sourcePath,valid,offset]);
  return <section aria-label="Saved-file charts">
    <div className="toolbar">
      <label>Frame class <select value={valid?"valid":"invalid"} onChange={e=>{setValid(e.target.value==="valid");setOffset(0);}}><option value="valid">Valid</option><option value="invalid">Invalid</option></select></label>
      <button disabled={loading || offset===0} onClick={()=>setOffset(Math.max(0,offset-200))}>Previous chart page</button>
      <button disabled={loading || !page || offset+200>=page.total} onClick={()=>setOffset(offset+200)}>Next chart page</button>
    </div>
    <p>Saved-file subset, not whole-file statistics: rows {page?.rows.length ? offset+1 : 0}–{offset+(page?.rows.length ?? 0)} of {page?.total ?? 0}. Raw pixel area; no live calibration or isoelastic overlays are applied.</p>
    {loading && <p role="status">Reading saved metrics…</p>}
    {error && <p role="alert">{error}</p>}
    <MonitoringCharts snapshot={page ? {valid:true,monitoring_active:false,valid_held:0,invalid_held:0,valid_appended:0,invalid_appended:0,capacity:200,latest_timestamp_ns:0,rows:page.rows} : null}/>
  </section>;
}
