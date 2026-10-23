// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { beforeEach, afterEach, expect, it, vi } from "vitest";
import { AnalysisWorkspace } from "./AnalysisWorkspace";
Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let root: Root, host: HTMLDivElement;
const read = vi.fn(), write = vi.fn(), job = vi.fn();
const graph = JSON.stringify({id:"plan",revision:"draft",output:"gate",nodes:[]});
const snapshot = {status:"ok",source:{fingerprint:"sha256:test",sourceKey:"test",parameters:{area:"pixel2"},scope:"stored"}};
const journal = "mib.analysis.pending-command.v1";
beforeEach(async () => {
  vi.resetAllMocks();
  const storage = new Map<string,string>();
  Object.defineProperty(window,"localStorage",{configurable:true,value:{
    getItem:(k:string)=>storage.get(k)??null, setItem:vi.fn((k:string,v:string)=>{storage.set(k,v);}),
    removeItem:(k:string)=>storage.delete(k), clear:()=>storage.clear()}});
  host=document.createElement("div");document.body.append(host);root=createRoot(host);
  read.mockResolvedValue(snapshot); write.mockResolvedValue({status:"ok",revision:"1"});
  await act(async()=>root.render(<AnalysisWorkspace path="a.h5" disabled={false} graphText={graph} feature="area" read={read} write={write} job={job}/>));
});
afterEach(async()=>{vi.restoreAllMocks(); await act(async()=>root.unmount());host.remove();});
async function click(text: string) { await act(async()=>Array.from(host.querySelectorAll("button")).find(b=>b.textContent===text)!.click()); }
it("journals exact source-bound command before the backend write and clears only after receipt",async()=>{
  write.mockImplementation(async command=>{expect(JSON.parse(window.localStorage.getItem(journal)!)).toEqual(command);return {status:"ok",revision:"1"};});
  await click("Save backend revision");
  expect(write.mock.calls[0][0].sourceBinding).toEqual(snapshot.source);
  expect(window.localStorage.getItem(journal)).toBeNull(); expect(host.textContent).toContain("Backend revision 1 saved");
});
it("does not write if the recovery journal cannot be persisted",async()=>{
  vi.spyOn(window.localStorage,"setItem").mockImplementation(()=>{throw new Error("disk full");});
  await click("Save backend revision"); expect(write).not.toHaveBeenCalled();expect(host.textContent).toContain("disk full");
});
it("lost reply keeps exact command and reconciliation does not resolve a new source",async()=>{
  write.mockRejectedValueOnce(new Error("connection lost"));await click("Save backend revision");
  const pending=window.localStorage.getItem(journal);expect(pending).not.toBeNull();
  read.mockClear();write.mockResolvedValue({status:"ok",revision:"1"});await click("Reconcile pending save");
  expect(read).not.toHaveBeenCalled();expect(write.mock.calls[1][0]).toEqual(JSON.parse(pending!));expect(window.localStorage.getItem(journal)).toBeNull();
});
it("keeps CAS revision during same-plan edits but clears it for a different plan",async()=>{
  await click("Save backend revision");
  await act(async()=>root.render(<AnalysisWorkspace path="a.h5" disabled={false} graphText={JSON.stringify({...JSON.parse(graph),revision:"edited"})} feature="area" read={read} write={write} job={job}/>));
  await click("Save backend revision"); expect(write.mock.calls[1][0].expectedRevision).toBe("1");
  await act(async()=>root.render(<AnalysisWorkspace path="a.h5" disabled={false} graphText={JSON.stringify({...JSON.parse(graph),id:"other"})} feature="area" read={read} write={write} job={job}/>));
  await click("Save backend revision"); expect(write.mock.calls[2][0].expectedRevision).toBeNull();
});
it("loads exact saved provenance and preserves it on the next revision",async()=>{
  const onLoadGraph=vi.fn();
  await act(async()=>root.render(<AnalysisWorkspace path="a.h5" disabled={false} graphText={graph} feature="area" read={read} write={write} job={job} onLoadGraph={onLoadGraph}/>));
  await click("Save backend revision");
  const method={method_id:"m",revision_id:"r",content_hash:"h"},application={planId:"template",revision:"2",overrides:[]};
  read.mockResolvedValueOnce({status:"ok",document:{graph:JSON.parse(graph),feature:"area",method,application,transforms:[],qc:{policy:"all",decisions:[]}}});
  await click("Load saved graph into editor"); expect(onLoadGraph).toHaveBeenCalledWith(JSON.parse(graph));
  expect(read.mock.calls[read.mock.calls.length-1][0]).toEqual({action:"read",planId:"plan",revision:"1"});
  await click("Save backend revision"); expect(write.mock.calls[write.mock.calls.length-1][0]).toMatchObject({expectedRevision:"1",method,application,qc:{policy:"all",decisions:[]}});
});

it("refuses to overwrite a selected but unloaded revision",async()=>{
  const input=host.querySelector<HTMLInputElement>('[aria-label="Strategy revision"]')!;
  await act(async()=>{Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,"value")!.set!.call(input,"9");input.dispatchEvent(new Event("input",{bubbles:true}));});
  await click("Save backend revision");expect(write).not.toHaveBeenCalled();expect(host.textContent).toContain("Load the selected saved graph");
});
