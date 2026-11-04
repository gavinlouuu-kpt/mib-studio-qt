interface Artifact {spec:{engine:string;features?:string[];seed:number;perplexity:number;maxIter:number;maxPoints:number};eligible:number;missing_features?:number;points:Array<{key:string;x:number;y:number}>}
/** Display only. Coordinates and exact-key sampling are produced and validated by the worker host. */
export function AnalysisEmbedding({artifact}:{artifact:Artifact}) {
 const xs=artifact.points.map(p=>p.x),ys=artifact.points.map(p=>p.y);
 const left=Math.min(...xs),right=Math.max(...xs),bottom=Math.min(...ys),top=Math.max(...ys);
 return <section aria-label="Pinned seeded embedding"><h5>Seeded t-SNE · {artifact.spec.engine}</h5>
  <p>{artifact.points.length} of {artifact.eligible} eligible events embedded; {artifact.missing_features ?? 0} selected events lack requested features. Sampling ranks SHA256(seed|exact event key), ascending; no implicit feature scaling. Features: {artifact.spec.features?.join(", ") || "job x/y parameters"}. Seed {artifact.spec.seed}, perplexity {artifact.spec.perplexity}, maximum iterations {artifact.spec.maxIter}. Coordinates are exploratory, not gates, cluster labels or evidence of an effect.</p>
  <svg viewBox="0 0 640 420" role="img" aria-label="Seeded t-SNE event map" style={{width:"min(100%,640px)",background:"white"}}>
   {artifact.points.map(p=><circle key={p.key} cx={20+600*(p.x-left)/(right-left||1)} cy={400-380*(p.y-bottom)/(top-bottom||1)} r="3" fill="#3973b9"><title>{p.key}</title></circle>)}
  </svg>
 </section>;
}
