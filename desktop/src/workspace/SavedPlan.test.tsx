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
beforeEach(() => { call.mockReset(); Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true }); host = document.createElement("div"); document.body.append(host); root = createRoot(host); });
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
