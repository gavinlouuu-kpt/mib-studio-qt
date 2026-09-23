// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { invoke } from "@tauri-apps/api/core";
import { open } from "@tauri-apps/plugin-dialog";
import { PreviewBufferControls, usePreviewBuffer } from "./previewBuffer";
vi.mock("@tauri-apps/api/core", () => ({invoke: vi.fn()}));
vi.mock("@tauri-apps/plugin-dialog", () => ({open: vi.fn()}));
Object.assign(globalThis, {IS_REACT_ACT_ENVIRONMENT: true});
let root: Root, host: HTMLDivElement, model: ReturnType<typeof usePreviewBuffer>;
const seek = vi.fn();
let visible = true;
function Harness() { model = usePreviewBuffer(true, false, seek); return visible ? <PreviewBufferControls model={model}/> : null; }
beforeEach(async () => {
  vi.resetAllMocks(); visible = true;
  vi.mocked(invoke).mockResolvedValue({available: true, first: "9007199254740993", last: "9007199254740995", count: "3", capture_running: false});
  vi.mocked(open).mockResolvedValue("/output");
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
  await act(async () => root.render(<Harness/>));
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });
it("preserves exact large frame identities while pausing and following live", async () => {
  await act(async () => model.select(model.range!.last));
  expect(seek).toHaveBeenLastCalledWith("9007199254740995");
  expect(host.textContent).toContain("frame 9007199254740995");
  await act(async () => model.select(null)); expect(seek).toHaveBeenLastCalledWith(null);
});
it("revalidates capture state after the output picker", async () => {
  vi.mocked(invoke).mockResolvedValue({available: true, capture_running: true});
  await act(async () => model.save());
  expect(vi.mocked(invoke).mock.calls.some(([name]) => name === "save_preview_buffer")).toBe(false);
  expect(model.message).toContain("Stop capture");
});
it("retains save ownership across navigation and reports partial output", async () => {
  let finish!: (value: unknown) => void;
  vi.mocked(invoke).mockImplementation(async name => name === "save_preview_buffer" ? new Promise(r => {finish = r;}) : {available: true, first: "1", last: "3", count: "3", capture_running: false});
  let pending!: Promise<void>;
  await act(async () => { pending = model.save(); });
  await act(async () => { visible = false; root.render(<Harness/>); await model.save(); });
  expect(vi.mocked(invoke).mock.calls.filter(([name]) => name === "save_preview_buffer")).toHaveLength(1);
  await act(async () => {finish({ok:false, output_path:"/output/partial", error:"disk full"}); await pending;});
  expect(model.message).toContain("partial output: /output/partial"); expect(model.busy).toBe(false);
});
