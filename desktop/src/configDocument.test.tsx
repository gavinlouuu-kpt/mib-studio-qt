// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { invoke } from "@tauri-apps/api/core";
import { open } from "@tauri-apps/plugin-dialog";
import { ConfigDocumentEditor, useConfigDocument, type ConfigDocument, type ConfigTransaction } from "./configDocument";
vi.mock("@tauri-apps/api/core", () => ({ invoke: vi.fn() }));
vi.mock("@tauri-apps/plugin-dialog", () => ({ open: vi.fn() }));
Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
const loaded: ConfigDocument = { ok: true, path: "/tmp/app.json", revision: "baseline-1", document_json: '{"image_processing":{"area_threshold_min":60}}', error: "" };
const success: ConfigTransaction = { saved: true, applied: true, verified: true, conflict: false, revision: "saved-2", error: "" };
let host: HTMLDivElement, root: Root, visible: boolean;
let model: ReturnType<typeof useConfigDocument>;
let ctx: Parameters<typeof useConfigDocument>[0];
function Harness() { model = useConfigDocument(ctx); return visible ? <ConfigDocumentEditor model={model} /> : <p>Another tab</p>; }
const button = (name: string) => [...host.querySelectorAll("button")].find(b => b.textContent === name)!;
async function click(name: string) { await act(async () => button(name).click()); }
async function edit(value = '{"area_threshold_min":80}') {
  await act(async () => {
    const input = host.querySelector("textarea")!;
    Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, "value")!.set!.call(input, value);
    input.dispatchEvent(new Event("input", {bubbles: true}));
  });
}
const mutations = () => vi.mocked(invoke).mock.calls.filter(([name]) => name === "apply_config_document");
beforeEach(async () => {
  vi.resetAllMocks(); visible = true;
  vi.spyOn(window, "confirm").mockReturnValue(true);
  vi.mocked(open).mockResolvedValue(loaded.path);
  vi.mocked(invoke).mockImplementation(async name => name === "fetch_config_document" ? loaded : success);
  ctx = { ready: true, active: false, append: vi.fn(), refresh: vi.fn().mockResolvedValue(undefined) };
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
  await act(async () => root.render(<Harness />)); await click("Open Config…");
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.restoreAllMocks(); });
describe("checked configuration operator transactions", () => {
  it("retains draft and baseline after unknown invoke failure without retry or optimistic success", async () => {
    await edit(); vi.mocked(invoke).mockRejectedValue(new Error("bridge disconnected; outcome unknown"));
    await click("Save and Apply");
    expect(model.dirty).toBe(true); expect(model.doc?.revision).toBe("baseline-1");
    expect(host.querySelector("textarea")?.value).toContain("80");
    expect(host.querySelector('[role="status"]')?.textContent).toContain("outcome unknown");
    expect(ctx.refresh).not.toHaveBeenCalled(); expect(mutations()).toHaveLength(1);
  });
  it("preserves conflicting edits and original baseline until explicit reload", async () => {
    await edit();
    vi.mocked(invoke).mockResolvedValue({...success, saved: false, applied: false, verified: false, conflict: true, revision: "external-2", error: "changed on disk"});
    await click("Save and Apply");
    expect(model.doc?.revision).toBe("baseline-1"); expect(model.dirty).toBe(true); expect(host.textContent).toContain("Conflict");
    await act(async () => root.render(<Harness />)); expect(mutations()).toHaveLength(1);
    vi.mocked(invoke).mockResolvedValue({...loaded, revision: "external-2", document_json: '{"image_processing":{"area_threshold_min":90}}'});
    await click("Reload / Revert");
    expect(window.confirm).toHaveBeenCalledOnce(); expect(model.doc?.revision).toBe("external-2");
    expect(model.dirty).toBe(false); expect(host.querySelector("textarea")?.value).toContain("90"); expect(mutations()).toHaveLength(1);
  });
  it("keeps saved-but-not-applied changes dirty and never claims runtime verification", async () => {
    await edit(); vi.mocked(invoke).mockResolvedValue({...success, applied: false, verified: false, error: "Saved; runtime application failed"});
    await click("Save and Apply");
    expect(model.dirty).toBe(true); expect(model.doc?.revision).toBe("baseline-1"); expect(model.result?.saved).toBe(true);
    expect(host.textContent).toContain("applied: false; verified: false"); expect(ctx.refresh).not.toHaveBeenCalled();
  });
  it("retains unsaved edits across navigation when App retains the hook", async () => {
    await edit(); visible = false; await act(async () => root.render(<Harness />)); expect(host.querySelector("textarea")).toBeNull();
    visible = true; await act(async () => root.render(<Harness />));
    expect(host.querySelector("textarea")?.value).toContain("80"); expect(model.dirty).toBe(true);
    expect(model.doc?.revision).toBe("baseline-1"); expect(mutations()).toHaveLength(0);
  });
  it("honors cancelled reload and clears dirty only after confirmed application", async () => {
    await edit(); vi.mocked(window.confirm).mockReturnValue(false); const reads = vi.mocked(invoke).mock.calls.length;
    await click("Reload / Revert"); expect(vi.mocked(invoke).mock.calls).toHaveLength(reads); expect(model.dirty).toBe(true);
    await click("Save and Apply"); expect(model.dirty).toBe(false); expect(model.doc?.revision).toBe("saved-2"); expect(ctx.refresh).toHaveBeenCalledOnce();
    expect(mutations()[0][1]).toEqual({path: loaded.path, baseline: "baseline-1", patch: '{"image_processing":{"area_threshold_min":80}}'});
  });
  it("preserves untouched additive defaults and advances the comparison baseline after each save", async () => {
    const initial = {area_threshold_min: 60, filters: {enable_multiple_contours_check: true, enable_border_check: true}};
    vi.mocked(invoke).mockResolvedValue({...loaded, document_json: JSON.stringify({image_processing: initial})});
    await click("Reload / Revert");
    vi.mocked(invoke).mockResolvedValue(success);
    await edit(JSON.stringify({...initial, area_threshold_min: 80}));
    await click("Save and Apply");
    expect(JSON.parse((mutations()[0][1] as {patch:string}).patch)).toEqual({image_processing:{area_threshold_min:80}});
    await edit(JSON.stringify({...initial, area_threshold_min:80, filters:{...initial.filters,enable_border_check:false}}));
    await click("Save and Apply");
    expect(mutations()[1][1]).toEqual({path:loaded.path,baseline:"saved-2",patch:'{"image_processing":{"filters":{"enable_border_check":false}}}'});
  });
  it("blocks duplicate pending mutations and locks editing during active runs", async () => {
    await edit(); let finish!: (r: ConfigTransaction) => void;
    vi.mocked(invoke).mockReturnValue(new Promise(r => { finish = r; }));
    await click("Save and Apply"); await click("Save and Apply"); expect(mutations()).toHaveLength(1); expect(button("Reload / Revert").disabled).toBe(true);
    await act(async () => finish(success)); ctx = {...ctx, active: true}; await act(async () => root.render(<Harness />));
    expect(button("Open Config…").disabled).toBe(true); expect(host.querySelector("textarea")?.disabled).toBe(true);
  });
});
