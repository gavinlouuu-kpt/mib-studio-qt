import type { MonitoringRow, MonitoringSnapshot } from "../bridge";

const LIMIT = 200;
export function monitoringPlotData(rows: MonitoringRow[]) {
  const bounded = rows.slice(-LIMIT);
  const scatter = bounded.filter(r => Number.isFinite(r.area) && Number.isFinite(r.deformability));
  const ratios = bounded.map(r=>r.ring_ratio).filter(Number.isFinite);
  const extent = (values: number[]): [number,number] => {
    if (!values.length) return [0,1];
    const low=Math.min(...values),high=Math.max(...values);
    return low === high ? [low-0.5,high+0.5] : [low,high];
  };
  const x = extent(scatter.map(r=>r.area)), y=extent(scatter.map(r=>r.deformability)), range=extent(ratios);
  const bins=Array<number>(10).fill(0);
  for (const value of ratios) bins[Math.min(9,Math.max(0,Math.floor((value-range[0])/(range[1]-range[0])*10)))]++;
  return {scatter,x,y,range,bins,ratioCount:ratios.length,omitted:rows.length-bounded.length,invalid:bounded.length-scatter.length};
}
const compact = (value:number)=>Number(value.toPrecision(4)).toString();
export function MonitoringCharts({snapshot}:{snapshot:MonitoringSnapshot|null}) {
  const data=monitoringPlotData(snapshot?.valid ? snapshot.rows : []);
  const pointX=(n:number)=>45+(n-data.x[0])/(data.x[1]-data.x[0])*300;
  const pointY=(n:number)=>165-(n-data.y[0])/(data.y[1]-data.y[0])*135;
  const maxBin=Math.max(1,...data.bins);
  return <>
    <div className="config-group">
      <h5>Deformability vs area (raw px²)</h5>
      {!snapshot?.valid || !data.scatter.length ? <p>No finite monitoring samples available.</p> : <svg viewBox="0 0 390 210" role="img" aria-label={`Deformability versus pixel area, ${data.scatter.length} samples`}>
        <title>Current bounded monitoring samples; no calibration or identity-matched overlays applied</title>
        <path d="M45 25V165H350" fill="none" stroke="currentColor"/>
        <text x="45" y="185" fontSize="11" fill="currentColor">{compact(data.x[0])}</text>
        <text x="345" y="185" textAnchor="end" fontSize="11" fill="currentColor">{compact(data.x[1])}</text>
        <text x="40" y="36" textAnchor="end" fontSize="11" fill="currentColor">{compact(data.y[1])}</text>
        <text x="40" y="165" textAnchor="end" fontSize="11" fill="currentColor">{compact(data.y[0])}</text>
        <text x="195" y="205" textAnchor="middle" fontSize="11" fill="currentColor">Area (px²)</text>
        {data.scatter.map((r,index)=> r.valid
          ? <circle key={index} cx={pointX(r.area)} cy={pointY(r.deformability)} r="3" fill={r.target_group ? "#60a5fa" : "#4ade80"}><title>Valid{r.target_group ? " target" : ""}: area {r.area}, deformability {r.deformability}</title></circle>
          : <path key={index} d={`M${pointX(r.area)-3} ${pointY(r.deformability)-3}l6 6m-6 0l6 -6`} stroke="#fb7185"><title>Invalid: area {r.area}, deformability {r.deformability}</title></path>)}
      </svg>}
      <p>Circles: valid · crosses: invalid · blue: target group. Up to 200 rows; {data.invalid} non-finite pairs omitted.</p>
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
      <p>{data.ratioCount} finite samples. This is ring ratio, not a physical ring-width measurement.</p>
    </div>
  </>;
}
