import { useEffect, useId, useState } from "react";
import { bridge, type ReviewChartSnapshot } from "../bridge";

/** Whole-file backend aggregates. All valid objects contribute; no page sampling. */
export function ReviewCharts({sourcePath}:{sourcePath:string}) {
  const [snapshot,setSnapshot]=useState<ReviewChartSnapshot|null>(null);
  const [error,setError]=useState("");
  const [loading,setLoading]=useState(false);
  const [refresh,setRefresh]=useState(0);
  const [overlays,setOverlays]=useState(true);
  const clipId=useId();
  useEffect(()=>{
    let active=true;setSnapshot(null);setError("");
    if(!sourcePath)return;
    setLoading(true);
    void (async()=>{
      try {
        const next=await bridge.fetchReviewCharts();
        if(!next.valid)throw new Error(next.error || "Saved metrics unavailable");
        if(next.source_path!==sourcePath)throw new Error("Review source changed. Reload charts for the selected file.");
        if(active)setSnapshot(next);
      }catch(e){if(active)setError(String(e));}
      finally{if(active)setLoading(false);}
    })();
    return()=>{active=false;};
  },[sourcePath,refresh]);
  const maxDensity=Math.max(1,...(snapshot?.density.map(cell=>Number(cell[2])) ?? []));
  const maxBin=Math.max(1,...(snapshot?.histogram.map(Number) ?? []));
  const x=(area:number)=>55+(area-(snapshot?.area_range[0] ?? 0))/((snapshot?.area_range[1] ?? 1)-(snapshot?.area_range[0] ?? 0))*600;
  const y=(deform:number)=>430-(deform-(snapshot?.deform_range[0] ?? 0))/((snapshot?.deform_range[1] ?? 1)-(snapshot?.deform_range[0] ?? 0))*380;
  const label=(value:number)=>Number(value.toPrecision(5)).toString();
  return <section aria-label="Saved-file charts">
    <div className="toolbar"><button disabled={loading} onClick={()=>setRefresh(n=>n+1)}>Refresh full-file charts</button><label><input type="checkbox" checked={overlays} onChange={e=>setOverlays(e.target.checked)}/>Isoelastic overlays</label></div>
    {loading && <p role="status">Aggregating all saved valid-object metrics…</p>}
    {error && <p role="alert">{error}</p>}
    {snapshot && <>
      <p>All {snapshot.rows} valid-dataset rows: {snapshot.finite_points} finite scatter points; {snapshot.excluded_nonfinite} non-finite pairs omitted. Every finite point contributes to the {snapshot.resolution}×{snapshot.resolution} density grid; no sampling. Calibration: {snapshot.pixel_to_micron} µm/px (current backend setting, same as Qt).</p>
      <svg viewBox="0 0 700 490" role="img" aria-label={`Full-file calibrated scatter density, ${snapshot.finite_points} objects`}>
        <defs><clipPath id={clipId}><rect x="55" y="50" width="600" height="380"/></clipPath></defs>
        <text x="55" y="25" fill="currentColor">Deformability vs Area (µm²)</text>
        <path d="M55 45V430H660" fill="none" stroke="currentColor"/>
        <g clipPath={`url(#${clipId})`}>
          {snapshot.density.map(([cx,cy,count])=><rect key={`${cx}:${cy}`} x={55+cx/snapshot.resolution*600} y={430-(cy+1)/snapshot.resolution*380} width={600/snapshot.resolution+0.2} height={380/snapshot.resolution+0.2} fill="#4ade80" opacity={0.2+0.8*Math.log1p(Number(count))/Math.log1p(maxDensity)}><title>{count} objects in this cell</title></rect>)}
          {overlays && snapshot.curves.map((curve,i)=><polyline key={curve.modulus} points={curve.points.map(([area,deform])=>`${x(area)},${y(deform)}`).join(" ")} fill="none" stroke={`hsl(${i*47%360} 65% 60%)`} strokeWidth="1.5"><title>{curve.modulus} kPa</title></polyline>)}
        </g>
        <text x="55" y="452" fill="currentColor">{label(snapshot.area_range[0])}</text><text x="655" y="452" textAnchor="end" fill="currentColor">{label(snapshot.area_range[1])}</text>
        <text x="50" y="55" textAnchor="end" fontSize="12" fill="currentColor">{label(snapshot.deform_range[1])}</text><text x="50" y="430" textAnchor="end" fontSize="12" fill="currentColor">{label(snapshot.deform_range[0])}</text>
        <text x="350" y="480" textAnchor="middle" fill="currentColor">Area (µm²)</text>
      </svg>
      {overlays && <details><summary>Isoelastic reference conditions and legend</summary><p>{snapshot.curve_source}. These are fixed reference curves, not automatically matched to this recording's flow conditions.</p><p>{snapshot.curves.map((curve,i)=><span key={curve.modulus} style={{color:`hsl(${i*47%360} 65% 60%)`,marginRight:12}}>{curve.modulus} kPa</span>)}</p></details>}
      <svg viewBox="0 0 700 490" role="img" aria-label={`Full-file ring-ratio histogram, ${snapshot.histogram_samples} objects`}>
        <text x="55" y="25" fill="currentColor">Ring-ratio distribution (dimensionless)</text><path d="M55 45V430H660" fill="none" stroke="currentColor"/>
        {snapshot.histogram.map((count,index)=><rect key={index} x={55+index/snapshot.histogram.length*600} y={430-Number(count)/maxBin*380} width={Math.max(0.5,600/snapshot.histogram.length-1)} height={Number(count)/maxBin*380} fill="#60a5fa"><title>Bin {index+1}: {count} objects</title></rect>)}
        <text x="55" y="452" fill="currentColor">{label(snapshot.ring_range[0])}</text><text x="655" y="452" textAnchor="end" fill="currentColor">{label(snapshot.ring_range[1])}</text><text x="50" y="55" textAnchor="end" fontSize="12" fill="currentColor">{maxBin}</text>
        <text x="350" y="480" textAnchor="middle" fill="currentColor">Ring ratio</text>
      </svg>
      <p>{snapshot.histogram_samples} positive finite ratios. Bin width 0.5; values outside current configured thresholds are clamped into the edge bins, matching Qt. Export Charts writes full-resolution TIFFs through the shared transactional exporter.</p>
    </>}
  </section>;
}
