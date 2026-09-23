import {useEffect,useId,useState} from "react";
import {invoke} from "@tauri-apps/api/core";
import type { MonitoringRow, MonitoringSnapshot } from "../bridge";

const LIMIT = 200;
export function monitoringPlotData(rows: MonitoringRow[]) {
  const bounded = rows.slice(-LIMIT);
  const scatter = bounded.filter(r=>r.valid&&Number.isFinite(r.pixel_to_micron)&&(r.pixel_to_micron??0)>0)
    .map(r=>({...r,area:r.area*r.pixel_to_micron!*r.pixel_to_micron!}))
    .filter(r=>Number.isFinite(r.area)&&Number.isFinite(r.deformability));
  const uncalibrated=bounded.filter(r=>r.valid&&(!Number.isFinite(r.pixel_to_micron)||(r.pixel_to_micron??0)<=0)).length;
  const ratios = bounded.filter(r=>r.valid).map(r=>r.ring_ratio).filter(v=>Number.isFinite(v)&&v>0);
  const moduli = bounded.filter(r=>r.valid).map(r=>r.youngs_modulus).filter(v=>Number.isFinite(v)&&v>0);
  const extent = (values: number[]): [number,number] => {
    if (!values.length) return [0,1];
    const low=Math.min(...values),high=Math.max(...values);
    return low === high ? [low-0.5,high+0.5] : [low,high];
  };
  const x = extent(scatter.map(r=>r.area)), y=extent(scatter.map(r=>r.deformability)), range=extent(ratios);
  const bins=Array<number>(10).fill(0);
  for (const value of ratios) bins[Math.min(9,Math.max(0,Math.floor((value-range[0])/(range[1]-range[0])*10)))]++;
  const modulusRange=extent(moduli),modulusBins=Array<number>(10).fill(0);
  for(const value of moduli)modulusBins[Math.min(9,Math.max(0,Math.floor((value-modulusRange[0])/(modulusRange[1]-modulusRange[0])*10)))]++;
  return {scatter,uncalibrated,x,y,range,bins,ratioCount:ratios.length,modulusRange,modulusBins,modulusCount:moduli.length,omitted:rows.length-bounded.length,invalid:bounded.length-scatter.length};
}
const compact = (value:number)=>Number(value.toPrecision(4)).toString();
export function MonitoringCharts({snapshot}:{snapshot:MonitoringSnapshot|null}) {
  const data=monitoringPlotData(snapshot?.valid ? snapshot.rows : []);
  const clip=useId();
  const [reference,setReference]=useState<{curves:{modulus:number;points:[number,number][]}[];curve_source:string}|null>(null);
  const [referenceError,setReferenceError]=useState("");
  const [overlays,setOverlays]=useState(false);
  useEffect(()=>{let live=true;invoke<string>("fetch_monitoring_chart_reference").then(text=>{if(live)setReference(JSON.parse(text));}).catch(e=>{if(live)setReferenceError(String(e));});return()=>{live=false;};},[]);
  const pointX=(n:number)=>45+(n-data.x[0])/(data.x[1]-data.x[0])*300;
  const pointY=(n:number)=>165-(n-data.y[0])/(data.y[1]-data.y[0])*135;
  const maxBin=Math.max(1,...data.bins);
  return <>
    <div className="config-group">
      <h5>Deformability vs area (µm²)</h5>
      {!snapshot?.valid || !data.scatter.length ? <p>No calibrated finite valid-object samples available.</p> : <svg viewBox="0 0 390 210" role="img" aria-label={`Deformability versus calibrated area, ${data.scatter.length} samples`}>
        <title>Current bounded valid-object samples; each uses its exact analysis-time calibration</title>
        <path d="M45 25V165H350" fill="none" stroke="currentColor"/>
        <text x="45" y="185" fontSize="11" fill="currentColor">{compact(data.x[0])}</text>
        <text x="345" y="185" textAnchor="end" fontSize="11" fill="currentColor">{compact(data.x[1])}</text>
        <text x="40" y="36" textAnchor="end" fontSize="11" fill="currentColor">{compact(data.y[1])}</text>
        <text x="40" y="165" textAnchor="end" fontSize="11" fill="currentColor">{compact(data.y[0])}</text>
        <text x="195" y="205" textAnchor="middle" fontSize="11" fill="currentColor">Area (µm²)</text>
        <defs><clipPath id={clip}><rect x="45" y="25" width="305" height="140"/></clipPath></defs>
        {overlays&&reference&&<g clipPath={`url(#${clip})`}>{reference.curves.map((curve,index)=><polyline key={curve.modulus} points={curve.points.map(([area,deform])=>`${pointX(area)},${pointY(deform)}`).join(" ")} fill="none" stroke={`hsl(${index*47%360} 65% 55%)`} strokeWidth="1"><title>{curve.modulus} kPa reference</title></polyline>)}</g>}
        {data.scatter.map((r,index)=> r.valid
          ? <circle key={index} cx={pointX(r.area)} cy={pointY(r.deformability)} r="3" fill={r.target_group ? "#60a5fa" : "#4ade80"}><title>Valid{r.target_group ? " target" : ""}: area {r.area}, deformability {r.deformability}</title></circle>
          : <path key={index} d={`M${pointX(r.area)-3} ${pointY(r.deformability)-3}l6 6m-6 0l6 -6`} stroke="#fb7185"><title>Invalid: area {r.area}, deformability {r.deformability}</title></path>)}
      </svg>}
      <p>Valid objects only; blue: target group. Up to 200 rows; {data.uncalibrated} valid rows lack analysis-time calibration and are omitted. Historical rows are never recalibrated with current settings.</p>
      <label><input type="checkbox" checked={overlays} disabled={!reference} onChange={e=>setOverlays(e.target.checked)}/>Show bundled isoelastic reference curves</label>
      {overlays&&reference&&<p>{reference.curve_source}. Reference conditions are not inferred from current hardware settings.</p>}
      {referenceError&&<p>Isoelastic reference unavailable: {referenceError}</p>}
    </div>
    <div className="config-group">
      <h5>Ring-ratio distribution (dimensionless)</h5>
      {!snapshot?.valid || !data.ratioCount ? <p>No finite ring-ratio samples available.</p> : <svg viewBox="0 0 390 210" role="img" aria-label={`Ring-ratio histogram, ${data.ratioCount} samples, ten bins`}>
        <path d="M45 25V165H350" fill="none" stroke="currentColor"/>
        {data.bins.map((count,index)=><rect key={index} x={46+index*30} y={165-count/maxBin*135} width="28" height={count/maxBin*135} fill="#60a5fa"><title>Bin {index+1}: {count} samples</title></rect>)}
        <text x="40" y="36" textAnchor="end" fontSize="11" fill="currentColor">{maxBin}</text>
        <text x="45" y="185" fontSize="11" fill="currentColor">{compact(data.range[0])}</text>
        <text x="345" y="185" textAnchor="end" fontSize="11" fill="currentColor">{compact(data.range[1])}</text>
        <text x="195" y="205" textAnchor="middle" fontSize="11" fill="currentColor">Ring ratio</text>
      </svg>}
      <p>{data.ratioCount} valid-object positive finite samples. This is ring ratio, not a physical ring-width measurement.</p>
    </div>
    <div className="config-group">
      <h5>Young’s modulus distribution (kPa)</h5>
      {!snapshot?.valid||!data.modulusCount?<p>No positive finite modulus measurements available for valid objects.</p>:<svg viewBox="0 0 390 210" role="img" aria-label={`Young's modulus histogram, ${data.modulusCount} valid-object samples, ten bins`}>
        <path d="M45 25V165H350" fill="none" stroke="currentColor"/>
        {data.modulusBins.map((count,index)=><rect key={index} x={46+index*30} y={165-count/Math.max(1,...data.modulusBins)*135} width="28" height={count/Math.max(1,...data.modulusBins)*135} fill="#4ade80"><title>Bin {index+1}: {count} samples</title></rect>)}
        <text x="40" y="36" textAnchor="end" fontSize="11" fill="currentColor">{Math.max(1,...data.modulusBins)}</text>
        <text x="45" y="185" fontSize="11" fill="currentColor">{compact(data.modulusRange[0])}</text>
        <text x="345" y="185" textAnchor="end" fontSize="11" fill="currentColor">{compact(data.modulusRange[1])}</text>
        <text x="195" y="205" textAnchor="middle" fontSize="11" fill="currentColor">Young’s modulus (kPa)</text>
      </svg>}
      <p>Stored per-object LUT result; no recalculation with current calibration. Zero/unavailable and non-finite values excluded.</p>
    </div>
  </>;
}
