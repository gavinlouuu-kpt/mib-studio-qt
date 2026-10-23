import { useRef } from "react";
export interface HistogramArtifact {
  planId: string; revision: string; feature: string; unit: string; sourceFingerprint?: string; sourceSetFingerprint?: string;
  edges: number[]; spec: {normalization: string}; semantics: string;
  layers: Array<{population: string;displayOffset: number;values: Array<number|null>;counts: string[];underflow: string;overflow: string;missing: string;unknown: string}>;
}
/** Presentation only: bins, normalization and membership are supplied by the native evaluator. */
export interface ScatterArtifact {
  planId:string;revision:string;sourceFingerprint:string;semantics:string;eligible:string;
  spec:{kind:"scatter";x:string;y:string;seed:number;maxPoints:number};xUnit:string;yUnit:string;
  bounds:{x:[number,number];y:[number,number]}|null;
  points:Array<{population:string;key:string;x:number;y:number}>;
  layers:Array<{population:string;selected:string;unknown:string;eligible:string;missingCoordinates:string}>;
}
export type PlotArtifact=HistogramArtifact|ScatterArtifact;
export function AnalysisPlot({artifact}:{artifact:PlotArtifact}){
  return "points" in artifact ? <ScatterPlot artifact={artifact}/> : <HistogramPlot artifact={artifact}/>;
}
function ScatterPlot({artifact}:{artifact:ScatterArtifact}){
  const palette=["#4f8cff","#ed9355","#59bd91","#bf83df","#e1bf4e","#dc7191","#67c0ca","#9aab7b"];
  const project=(values:number[],length:number)=>{if(!values.length)return ()=>length/2;const scale=Math.max(...values.map(Math.abs))||1;const normalized=values.map(v=>v/scale);const low=Math.min(...normalized),high=Math.max(...normalized);return (v:number)=>length*(v/scale-low)/(high-low||1);};
  const x=project(artifact.bounds?.x??[],600),y=project(artifact.bounds?.y??[],360);
  return <section aria-label="Saved population scatter"><p>{artifact.points.length} displayed of {artifact.eligible} eligible layer/event pairs. {artifact.semantics}</p>
    <svg viewBox="0 0 700 450" role="img" aria-label="Saved raw or derived parameter scatter" style={{width:"min(100%,700px)",background:"white"}}>
      <path d="M60 20V380H660" fill="none" stroke="#555"/>
      {artifact.points.map(p=><circle key={JSON.stringify([p.population,p.key])} cx={60+x(p.x)} cy={380-y(p.y)} r="3" fill={palette[artifact.layers.findIndex(l=>l.population===p.population)]}><title>{p.population}: {p.key}; {p.x}, {p.y}</title></circle>)}
      {artifact.bounds && <><text x="60" y="400" fontSize="11">{artifact.bounds.x[0].toPrecision(4)}</text><text x="660" y="400" textAnchor="end" fontSize="11">{artifact.bounds.x[1].toPrecision(4)}</text></>}
      <text x="360" y="420" textAnchor="middle">{artifact.spec.x} ({artifact.xUnit})</text><text x="10" y="20">{artifact.spec.y} ({artifact.yUnit})</text>
    </svg>
    <ul>{artifact.layers.map((l,i)=><li key={l.population} style={{color:palette[i]}}>{l.population}: {l.selected} selected; {l.unknown} membership unknown; {l.missingCoordinates} missing coordinates</li>)}</ul>
    <p>Plan {artifact.planId}, revision {artifact.revision}; seed {artifact.spec.seed}; source {artifact.sourceFingerprint}.</p>
  </section>;
}
function HistogramPlot({artifact}: {artifact: HistogramArtifact}) {
  const svg=useRef<SVGSVGElement>(null);
  const palette=["#4f8cff","#ed9355","#59bd91","#bf83df","#e1bf4e","#dc7191","#67c0ca","#9aab7b"];
  const displayed=artifact.layers.flatMap(l=>l.values.flatMap(v=>v===null?[]:[v+l.displayOffset]));
  const low=Math.min(0,...displayed), high=Math.max(0,...displayed), span=high-low || 1;
  const y=(v:number)=>240-(v-low)/span*210;
  const x=(i:number)=>60+i/(artifact.edges.length-1)*600;
  return <section aria-label="Saved population overlay">
    <svg ref={svg} xmlns="http://www.w3.org/2000/svg" viewBox="0 0 720 300" role="img" aria-label={`${artifact.feature} population histogram`} style={{width:"min(100%, 720px)",background:"white",color:"#222"}}>
      <title>Plan {artifact.planId} revision {artifact.revision}</title>
      <desc>{artifact.semantics} {artifact.sourceSetFingerprint ? `Source set ${artifact.sourceSetFingerprint}` : `Source ${artifact.sourceFingerprint}`}</desc>
      <path d="M60 20V240H660" fill="none" stroke="#555"/>
      <text x="60" y="260" fontSize="12">{artifact.edges[0]}</text><text x="660" y="260" textAnchor="end" fontSize="12">{artifact.edges[artifact.edges.length-1]}</text>
      <text x="360" y="287" textAnchor="middle" fontSize="13">{artifact.feature} ({artifact.unit})</text>
      <text x="55" y="25" textAnchor="end" fontSize="11">{high.toPrecision(4)}</text><text x="55" y="240" textAnchor="end" fontSize="11">{low.toPrecision(4)}</text>
      {artifact.layers.map((layer,i)=>{
        let connected=false;
        const d=layer.values.map((value,bin)=>{
          if(value===null){connected=false;return "";}
          const segment=`${connected?"L":"M"}${x(bin)} ${y(value+layer.displayOffset)}L${x(bin+1)} ${y(value+layer.displayOffset)}`;
          connected=true;return segment;
        }).join(" ");
        return <path key={layer.population} d={d} stroke={palette[i]} fill="none" strokeWidth="2"/>;
      })}
    </svg>
    <p>Y: {artifact.spec.normalization}; displayed offsets are not scientific changes. {artifact.semantics}</p>
    <ul>{artifact.layers.map((l,i)=><li key={l.population} style={{color:palette[i]}}>{l.population}: offset {l.displayOffset}; underflow {l.underflow}, overflow {l.overflow}, missing {l.missing}, membership unknown {l.unknown}</li>)}</ul>
    <button onClick={()=>{
      if(!svg.current)return;
      const blob=new Blob([svg.current.outerHTML],{type:"image/svg+xml"});const url=URL.createObjectURL(blob);
      const a=document.createElement("a");a.href=url;a.download="population-overlay.svg";a.click();URL.revokeObjectURL(url);
    }}>Download pinned SVG</button>
  </section>;
}
