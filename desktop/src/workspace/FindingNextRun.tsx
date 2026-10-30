import { useState } from "react";
import { Button } from "@mib/ui";
import { mintExperimentId } from "./planModel";

/** Explicit planning only. Evidence is resolved by native providers, never this form. */
export function FindingNextRun({ workspaceId, revision, profileBase, blocked, controlHeld, evidence, request }: {
  workspaceId:string; revision:string; profileBase:string; blocked:boolean; controlHeld:boolean;
  evidence:unknown; request:(write:boolean,payload:string)=>Promise<void>;
}) {
  const [findingId,setFindingId]=useState("");
  const [hash,setHash]=useState("");
  const [sample,setSample]=useState("0");
  const valid=!!findingId && /^sha256:[0-9a-f]{64}$/.test(hash) && /^(0|[1-9][0-9]*)$/.test(sample) && Number(sample)<32;
  const reference={findingId,revision:"1",contentHash:hash};
  return <details><summary>Plan next run from an immutable finding</summary>
    <p>Choose the finding and its zero-based sample index. Native validation checks retained sources, exact analysis and method pins, and content hashes. Creating a draft does not load settings, copy run results, or start acquisition. Compatibility remains unknown.</p>
    <label>Finding ID <input aria-label="Finding ID" value={findingId} disabled={blocked} onChange={e=>setFindingId(e.target.value)}/></label>
    <label>Finding content hash <input aria-label="Finding content hash" value={hash} disabled={blocked} onChange={e=>setHash(e.target.value)}/></label>
    <label>Finding sample index <input aria-label="Finding sample index" value={sample} disabled={blocked} onChange={e=>setSample(e.target.value)}/></label>
    <Button disabled={blocked||!valid} onClick={()=>void request(false,JSON.stringify({action:"resolve_finding",workspace_id:workspaceId,revision,finding:reference,sample_index:Number(sample),profile_base:profileBase}))}>Validate finding for planning</Button>
    <Button disabled={blocked||!valid||!controlHeld} onClick={()=>void request(true,JSON.stringify({action:"draft_from_finding",workspace_id:mintExperimentId(),command_id:mintExperimentId(),source_workspace_id:workspaceId,source_revision:revision,finding:reference,sample_index:Number(sample),profile_base:profileBase,experiment_id:mintExperimentId(),recipe_id:mintExperimentId()}))}>Validate and create next-run draft</Button>
    {evidence!==undefined && <><p>Recorded provenance below is historical, not a current validation result.</p><Button disabled={blocked} onClick={()=>void request(false,JSON.stringify({action:"verify_next_run",workspace_id:workspaceId,revision,profile_base:profileBase}))}>Verify recorded next-run evidence</Button><pre>{JSON.stringify(evidence,null,2)}</pre></>}
  </details>;
}
