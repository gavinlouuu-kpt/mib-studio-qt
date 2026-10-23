// @vitest-environment jsdom
import { act } from "react";
import { createRoot } from "react-dom/client";
import { expect, it, vi } from "vitest";
import { usePlan, type PlanController, type PlanStartResult } from "./usePlan";
import { newPlanDraft } from "./planModel";
it("loads a detached sheet under a fresh identity without starting; viewers cannot replace it", () => {
  Object.assign(globalThis,{IS_REACT_ACT_ENVIRONMENT:true});
  const host=document.createElement("div"), root=createRoot(host);
  const start=vi.fn(async ():Promise<PlanStartResult> => ({kind:"busy"}));
  let plan!:PlanController;
  function Harness({control=true}:{control?:boolean}) {plan=usePlan(start,{controlHeld:control});return null;}
  const saved=newPlanDraft("12345678-1234-1234-1234-123456789abc"); saved.rows[0].sample="Persisted";
  try {
    act(()=>root.render(<Harness/>));
    act(()=>{expect(plan.loadSaved!(saved)).toBe(true);});
    expect(plan.draft.experimentId).not.toBe(saved.experimentId);
    expect(plan.draft.rows).toEqual(saved.rows); expect(plan.draft.rows).not.toBe(saved.rows);
    expect(start).not.toHaveBeenCalled();
    const loaded=plan.draft;
    act(()=>root.render(<Harness control={false}/>));
    act(()=>{expect(plan.loadSaved!(saved)).toBe(false);});
    expect(plan.draft).toBe(loaded);
  } finally {act(()=>root.unmount());}
});
