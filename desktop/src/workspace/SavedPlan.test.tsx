// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { SavedPlan } from "./SavedPlan";
import { newPlanDraft } from "./planModel";
import { decodeSavedPlan } from "./savedPlanModel";
vi.mock("../transport", () => ({ invoke: vi.fn() }));
import { invoke } from "../transport";
const call = vi.mocked(invoke);
const draft = newPlanDraft("12345678-1234-1234-1234-123456789abc");
const ok = { status: "ok", workspace_id: draft.experimentId, revision: "1", document: { schema_version: 1, plan: draft } };
let host: HTMLDivElement, root: Root;
beforeEach(() => { sessionStorage.clear(); call.mockReset(); Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true }); host = document.createElement("div"); document.body.append(host); root = createRoot(host); });
afterEach(() => { act(() => root.unmount()); host.remove(); });
const render = (controlHeld = true) => act(() => root.render(<SavedPlan draft={draft} controlHeld={controlHeld} />));
const button = (text: string) => [...host.querySelectorAll("button")].find(b => b.textContent === text)!;
const click = async (text: string) => { await act(async () => { button(text).click(); }); };
it("does not act on mount; explicit save uses create-only CAS and reopen is a read", async () => {
  render(); expect(call).not.toHaveBeenCalled();
  call.mockResolvedValue(ok);
  await click("Save new workspace");
  expect(call.mock.calls[0][0]).toBe("workspace_write");
  const request = JSON.parse((call.mock.calls[0][1] as {request:string}).request);
  expect(request.expected_revision).toBeNull(); expect(request.document.plan).toEqual(draft);
  await click("Save new revision");
  expect(JSON.parse((call.mock.calls[1][1] as {request:string}).request).expected_revision).toBe("1");
  await click("Open saved plan"); expect(call.mock.calls[2][0]).toBe("workspace_read");
});
it("reconciles lost replies using byte-identical command and no blind overwrite", async () => {
  render(); call.mockRejectedValueOnce(new Error("disconnected")).mockResolvedValue(ok);
  await click("Save new workspace");
  expect(button("Save new workspace").disabled).toBe(true);
  await click("Retry same save");
  expect(call.mock.calls[1]).toEqual(call.mock.calls[0]);
  expect(host.textContent).toContain("Saved revision 1");
});
it("clears stale saved annotations on conflict and viewer cannot write", async () => {
  render(); call.mockResolvedValueOnce(ok).mockResolvedValueOnce({ status:"conflict", message:"Stale revision" });
  await click("Open saved plan"); await click("Save new revision");
  expect(host.textContent).not.toContain("immutable revision 1");
  expect(host.textContent).toContain("Stale revision");
  render(false); expect(button("Save new workspace").disabled).toBe(true);
});
it("rejects malformed persisted rows and returns detached copies", () => {
  const copy = decodeSavedPlan(ok.document); copy.rows[0].sample="changed";
  expect(draft.rows[0].sample).toBe("");
  expect(() => decodeSavedPlan({schema_version:1,plan:{...draft,rows:[{}]}})).toThrow();
  expect(() => decodeSavedPlan({schema_version:1,plan:{...draft,rows:[draft.rows[0],draft.rows[0]]}})).toThrow();
});
it("retains exact pending command across remount and never replays automatically", async () => {
  render(); call.mockRejectedValueOnce(new Error("lost reply"));
  await click("Save new workspace");
  const sent = call.mock.calls[0];
  act(() => root.unmount()); root = createRoot(host); render();
  expect(call).toHaveBeenCalledTimes(1);
  call.mockResolvedValue(ok); await click("Retry same save");
  expect(call.mock.calls[1]).toEqual(sent);
  expect(sessionStorage.getItem("mib.workspace.pending-save.v1")).toBeNull();
});
it("restores an immutable revision using the opened head as CAS", async () => {
  render(); call.mockResolvedValueOnce({...ok, revision:"4"}); await click("Open saved plan");
  call.mockResolvedValueOnce({status:"ok", revisions:["1","2","3","4"]}); await click("Read revision history");
  const select = host.querySelector("select")!;
  act(() => { select.value = "2"; select.dispatchEvent(new Event("change", {bubbles:true})); });
  call.mockResolvedValueOnce({...ok,revision:"5"}); await click("Restore selected revision");
  expect(JSON.parse((call.mock.calls[2][1] as {request:string}).request)).toMatchObject({action:"restore", expected_revision:"4",target_revision:"2"});
  expect(host.textContent).toContain("Saved revision 5");
});
it("loads only on explicit action and reports a locked controller refusal", async () => {
  const load = vi.fn(() => false);
  act(() => root.render(<SavedPlan draft={draft} controlHeld onLoad={load} />));
  call.mockResolvedValue(ok); await click("Open saved plan"); expect(load).not.toHaveBeenCalled();
  await click("Load as new experiment"); expect(load).toHaveBeenCalledWith(draft);
  expect(host.textContent).toContain("Cannot load");
});
it("preserves pinned recipe references through reopen and another save without claiming compatibility", async () => {
  const recipe = {schema_version:1,id:"recipe-a",revision:"8",title:"Pair",question:"Question",method:{source:"registry",id:"method-a",revision:"hash-a"},analysis_plan:{id:"analysis-a",revision:"3"},lineage:{recipe:null,finding:null}};
  render(); call.mockResolvedValue({...ok, document:{...ok.document,recipe}});
  await click("Open saved plan"); await click("Save new revision");
  const q=JSON.parse((call.mock.calls[1][1] as {request:string}).request);
  expect(q.document.recipe).toEqual(recipe);
  expect(host.textContent).toContain("Compatibility not checked");
});
it("does not send a write if reload recovery storage cannot be written", async () => {
  render(); const stub = vi.spyOn(Object.getPrototypeOf(sessionStorage),"setItem").mockImplementation(() => {throw new Error("quota");});
  try { await click("Save new workspace"); expect(call).not.toHaveBeenCalled(); expect(host.textContent).toContain("quota"); }
  finally {stub.mockRestore();}
});
