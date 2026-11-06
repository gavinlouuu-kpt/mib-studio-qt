// @vitest-environment jsdom
// #863 A2 (CONNECT-056..059, 061): the Overview's overviewConfig.js editor, driven through its buttons with an in-memory
// file system behind the camera_document command: load (and make the default), the unsaved label, Reset, Save with
// "Saved.", Apply to Camera (save, stop, Overview role), Browse... and Clear with the persisted external path.
import { act, createElement } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { CameraSelection } from "../../bridge";

const BUNDLED = "// bundled overviewConfig.js\r\nvar g = grabbers[0];\r\n";
interface File { text: string; rev: number }
let files: Map<string, File>;
let calls: string[];

const bridgeMock = vi.hoisted(() => ({ appPaths: vi.fn(), fetchCameraSelection: vi.fn(), stopCapture: vi.fn(), applyCameraScriptWithRole: vi.fn() }));
const invokeMock = vi.hoisted(() => vi.fn());
const dialogs = vi.hoisted(() => ({ open: vi.fn(), ask: vi.fn(), confirm: vi.fn() }));
vi.mock("../../bridge", () => ({ bridge: bridgeMock }));
vi.mock("../../transport", () => ({ invoke: invokeMock }));
vi.mock("../../transport/dialogs", () => dialogs);
import { OverviewScriptEditor } from "./OverviewScriptEditor";
import { overviewScriptContext, useOverviewScript, type OverviewScriptContext, type OverviewScriptModel } from "./useOverviewScript";
import { MSG_APPLIED, MSG_CLEARED_QUESTION, OVERVIEW_SCRIPT_KEY } from "./overviewScript";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
const storage = new Map<string, string>();
Object.defineProperty(window, "localStorage", { configurable: true, value: {
  getItem: (k: string) => storage.get(k) ?? null, setItem: (k: string, v: string) => { storage.set(k, v); }, removeItem: (k: string) => { storage.delete(k); },
} });

const PROFILE = "/data/profiles/lab/config.json";
const PROFILE_SCRIPT = "/data/profiles/lab/overviewConfig.js";
const APP_SCRIPT = "/home/u/.config/app/overviewConfig.js";
const selection = (over: Partial<CameraSelection> = {}): CameraSelection => ({
  valid: true, configured: true, mode: 2, running: false, interface_index: 0, device_index: 0, label: "MC2066", mindvision_index: -1,
  mindvision_config_path: "", camera_script_path: "", mock_frame_dir: "", mock_interval_ms: 0, mock_loop: false, ...over,
});

let host: HTMLDivElement, root: Root, model: OverviewScriptModel, log: string[], running: boolean[], refreshed: number;
let ctx: OverviewScriptContext;
// (createElement, not JSX: the ViewerGate source scan looks for the editor's tag in the area sources and wants it gated in the real view.)
function Harness({ c }: { c: OverviewScriptContext }) {
  model = useOverviewScript(c);
  return createElement(OverviewScriptEditor, { model });
}
const render = async (over: Partial<OverviewScriptContext> = {}) => {
  ctx = { ...ctx, ...over };
  await act(async () => { root.render(<Harness c={ctx} />); });
  await act(async () => { await Promise.resolve(); });
};
const button = (name: string) => Array.from(host.querySelectorAll("button")).find((b) => b.textContent === name) as HTMLButtonElement;
const click = async (name: string) => { await act(async () => { button(name).click(); }); await act(async () => { await Promise.resolve(); }); };
const area = () => host.querySelector("textarea") as HTMLTextAreaElement;
const type = async (value: string) => {
  const set = Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, "value")!.set!;
  set.call(area(), value);
  await act(async () => { area().dispatchEvent(new Event("input", { bubbles: true })); });
};
const message = () => host.querySelector('[data-testid="overview-script-message"]')!.textContent;
const unsaved = () => host.querySelector('[data-testid="overview-script-unsaved"]');
const shownPath = () => host.querySelector('[data-testid="overview-script-path"]')!.textContent;
const seed = (path: string, text: string) => files.set(path, { text, rev: 1 });

beforeEach(() => {
  files = new Map(); calls = []; log = []; running = []; refreshed = 0; storage.clear();
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
  ctx = {
    ready: true, expActive: false, recording: false, eGrabberScripts: true, selection: selection(), profileConfigPath: PROFILE,
    append: (l) => log.push(l), refreshCameraState: async () => { calls.push("refresh state"); }, refreshGeometry: async () => { refreshed++; },
    setRunning: (r) => running.push(r),
  };
  for (const fn of [...Object.values(bridgeMock), invokeMock, ...Object.values(dialogs)]) fn.mockReset();
  bridgeMock.appPaths.mockResolvedValue({ app_config: "/home/u/.config/app" });
  bridgeMock.fetchCameraSelection.mockImplementation(async () => selection());
  bridgeMock.stopCapture.mockImplementation(async () => { calls.push("stop"); return { ok: true, message: "stopped" }; });
  bridgeMock.applyCameraScriptWithRole.mockImplementation(async (path: string, overview: boolean) => { calls.push(`apply ${overview ? "overview" : "experiment"} ${path}`); return { ok: true, message: "applied" }; });
  dialogs.confirm.mockResolvedValue(true);
  dialogs.ask.mockResolvedValue(true);
  // The camera_document command on an in-memory disk; the canonical path it answers with is a Windows-style \\?\ path, like the real one.
  invokeMock.mockImplementation(async (cmd: string, a: { action: string; path: string; kind: string; baseline: string; text: string }) => {
    expect(cmd).toBe("camera_document");
    expect(a.kind).toBe("overview");
    // The command canonicalizes: the \\?\ prefix it answers with on Windows is accepted back as the same file.
    const key = a.path.replace(/^\\\\\?\\/, "");
    calls.push(`${a.action} ${key}`);
    if (a.action === "default") return { path: "", text: BUNDLED, revision: "" };
    const f = files.get(key);
    const out = (file: File) => ({ path: `\\\\?\\${key}`, text: file.text, revision: `r${file.rev}` });
    if (a.action === "read") { if (!f) throw new Error("No such file or directory (os error 2)"); return out(f); }
    if (a.action === "create") { if (f) throw new Error("File exists (os error 17)"); files.set(key, { text: a.text, rev: 1 }); return out(files.get(key)!); }
    if (a.action === "save") {
      if (!f) throw new Error("No such file or directory (os error 2)");
      if (a.baseline !== `r${f.rev}`) throw new Error("Camera file changed on disk; reload and reconcile your draft");
      f.text = a.text; f.rev++; return out(f);
    }
    throw new Error("unexpected action");
  });
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

describe("loading (CONNECT-056)", () => {
  it("shows the script beside the active profile's config.json with its path", async () => {
    seed(PROFILE_SCRIPT, "// my full-sensor script\n");
    await render();
    expect(shownPath()).toBe(PROFILE_SCRIPT);
    expect(area().value).toBe("// my full-sensor script\n");
    expect(area().disabled).toBe(false);
    expect(unsaved()).toBeNull();
    expect(model.path).toBe(PROFILE_SCRIPT);            // what the stage change applies
    expect(model.defaultPath).toBe(PROFILE_SCRIPT);
  });

  it("makes the default file from the bundled overviewConfig.js when it is missing, and says so in the log", async () => {
    await render();
    expect(files.get(PROFILE_SCRIPT)?.text).toBe(BUNDLED);
    expect(calls).toEqual([`read ${PROFILE_SCRIPT}`, "default ", `create ${PROFILE_SCRIPT}`]);
    expect(area().value.replace(/\r\n/g, "\n")).toBe(BUNDLED.replace(/\r\n/g, "\n"));
    expect(log.join("\n")).toContain("created from the bundled default");
  });

  it("uses the app config folder when no profile is applied, and moves to the profile's file once one is (no edits to lose)", async () => {
    seed(PROFILE_SCRIPT, "// profile\n");
    await render({ profileConfigPath: "" });
    expect(shownPath()).toBe(APP_SCRIPT);
    expect(model.defaultPath).toBe(APP_SCRIPT);
    expect(files.get(APP_SCRIPT)?.text).toBe(BUNDLED);
    await render({ profileConfigPath: PROFILE });
    expect(shownPath()).toBe(PROFILE_SCRIPT);
    expect(area().value).toBe("// profile\n");
  });

  it("keeps the operator's edits when a profile is applied meanwhile: Save waits until Reset has loaded the new file", async () => {
    seed(APP_SCRIPT, "// app\n"); seed(PROFILE_SCRIPT, "// profile\n");
    await render({ profileConfigPath: "" });
    await type("// edited\n");
    await render({ profileConfigPath: PROFILE });
    expect(area().value).toBe("// edited\n");
    expect(unsaved()).not.toBeNull();
    expect(button("Save").disabled).toBe(true);
    expect(button("Apply to Camera").disabled).toBe(true);
    expect(model.path).toBe(PROFILE_SCRIPT);
    await click("Reset");
    expect(dialogs.confirm).toHaveBeenCalledWith("Discard the unsaved changes to the Overview script?");
    expect(area().value).toBe("// profile\n");
    expect(button("Save").disabled).toBe(false);
  });

  it("says why a script cannot be loaded and leaves the editor off", async () => {
    invokeMock.mockImplementation(async () => { throw new Error("Permission denied (os error 13)"); });
    await render();
    expect(message()).toBe("Failed to load: Permission denied (os error 13)");
    expect(host.querySelector('[role="alert"]')).not.toBeNull();
    expect(area().disabled).toBe(true);
    expect(button("Save").disabled).toBe(true);
    expect(invokeMock.mock.calls.filter((c) => c[1].action === "read")).toHaveLength(1); // no retry loop
  });

  it("waits for the backend", async () => {
    await render({ ready: false });
    expect(invokeMock).not.toHaveBeenCalled();
    await render({ ready: true });
    expect(invokeMock).toHaveBeenCalled();
  });
});

describe("unsaved changes, Reset and Save (CONNECT-056, 057)", () => {
  beforeEach(() => { seed(PROFILE_SCRIPT, "line1\r\nline2\r\n"); });

  it("shows Qt's 'Unsaved changes – click Save to apply.' on an edit and drops it when the text is back to the file's", async () => {
    await render();
    await type("line1\nchanged\n");
    expect(unsaved()?.textContent).toBe("Unsaved changes – click Save to apply.");
    await type("line1\nline2\n");   // the textarea reports LF for the file's CRLF
    expect(unsaved()).toBeNull();
  });

  it("Reset reloads the file; with unsaved edits it asks first and does nothing on No", async () => {
    await render();
    await type("scribble");
    dialogs.confirm.mockResolvedValueOnce(false);
    await click("Reset");
    expect(area().value).toBe("scribble");
    await click("Reset");
    expect(area().value.replace(/\r\n/g, "\n")).toBe("line1\nline2\n");
    expect(unsaved()).toBeNull();
    // clean text: no question
    dialogs.confirm.mockClear();
    await click("Reset");
    expect(dialogs.confirm).not.toHaveBeenCalled();
  });

  it("Reset makes the default file again when it was deleted (Qt ensureDefaultsFile on reload)", async () => {
    await render();
    files.delete(PROFILE_SCRIPT);
    await click("Reset");
    expect(files.get(PROFILE_SCRIPT)?.text).toBe(BUNDLED);
  });

  it("Save writes the file with the revision it read and confirms with 'Saved.'", async () => {
    await render();
    await type("line1\nedited\n");
    await click("Save");
    expect(files.get(PROFILE_SCRIPT)?.text).toBe("line1\r\nedited\r\n");  // the file's CRLF is kept
    expect(invokeMock).toHaveBeenCalledWith("camera_document", expect.objectContaining({ action: "save", baseline: "r1", kind: "overview" }));
    expect(message()).toBe("Saved.");
    expect(host.querySelector('[role="status"]')).not.toBeNull();
    expect(unsaved()).toBeNull();
    // the next edit goes with the confirmation, and a second save uses the new revision
    await type("line1\nedited again\n");
    expect(message()).toBe("");
    await click("Save");
    expect(files.get(PROFILE_SCRIPT)?.text).toBe("line1\r\nedited again\r\n");
    expect(message()).toBe("Saved.");
  });

  it("a Save the disk refuses (the file changed underneath) says so and keeps the text", async () => {
    await render();
    await type("mine\n");
    files.get(PROFILE_SCRIPT)!.rev++; // someone else saved
    await click("Save");
    expect(message()).toBe("Failed to save: Camera file changed on disk; reload and reconcile your draft");
    expect(host.querySelector('[role="alert"]')).not.toBeNull();
    expect(area().value).toBe("mine\n");
    expect(unsaved()).not.toBeNull();
    expect(files.get(PROFILE_SCRIPT)?.text).toBe("line1\r\nline2\r\n");
  });
});

describe("Apply to Camera (CONNECT-058)", () => {
  beforeEach(() => { seed(PROFILE_SCRIPT, "// full sensor\n"); });

  // #863 integration: the stop and the apply run under Start / Stop Camera's one-command guard, the rescan after them does not.
  it("holds the camera command guard for the stop and the apply only", async () => {
    bridgeMock.fetchCameraSelection.mockImplementation(async () => selection({ running: true }));
    const held: string[] = [];
    let holding = false;
    bridgeMock.stopCapture.mockImplementation(async () => { held.push(`stop:${holding}`); return { ok: true, message: "stopped" }; });
    bridgeMock.applyCameraScriptWithRole.mockImplementation(async () => { held.push(`apply:${holding}`); return { ok: true, message: "applied" }; });
    await render({
      holdCamera: async (work) => { holding = true; try { return await work(); } finally { holding = false; } },
      refreshCameraState: async () => { held.push(`refresh:${holding}`); },
    });
    await click("Apply to Camera");
    expect(held).toEqual(["stop:true", "apply:true", "refresh:false"]);
  });

  // The studio wiring: the context's holdCamera is Start / Stop Camera's guard itself (cameraCommandRef), kind "stage".
  it("the studio context holds Start / Stop Camera's own guard while the sequence runs", async () => {
    const ref: { current: "start" | "stop" | "stage" | null } = { current: null };
    const seen: Array<string | null> = [];
    const ctx = overviewScriptContext({
      ready: true, expActive: false, recording: false, caps: { egrabber_script: true }, camSelection: null, profiles: { activeProfile: null },
      append: () => undefined, refreshCameraState: async () => undefined, refreshCameraGeometry: async () => null, setRunning: () => undefined,
      cameraCommandRef: ref, setCameraCommand: (command: string | null) => { seen.push(command); },
    } as never);
    const result = await ctx.holdCamera!(async () => { seen.push(`work:${ref.current}`); return 7; });
    expect(result).toBe(7);
    expect(seen).toEqual(["stage", "work:stage", null]);
    expect(ref.current).toBeNull();
  });

  it("saves first, stops a running camera, applies with the Overview role, and leaves the capture stopped", async () => {
    bridgeMock.fetchCameraSelection.mockImplementation(async () => selection({ running: true }));
    await render();
    await type("// full sensor, edited\n");
    calls.length = 0;
    await click("Apply to Camera");
    expect(calls).toEqual([`save ${PROFILE_SCRIPT}`, "stop", `apply overview ${PROFILE_SCRIPT}`, "refresh state"]);
    expect(files.get(PROFILE_SCRIPT)?.text).toBe("// full sensor, edited\n");
    expect(bridgeMock.applyCameraScriptWithRole).toHaveBeenCalledWith(PROFILE_SCRIPT, true);
    expect(running).toEqual([false]);
    expect(refreshed).toBe(1);
    expect(message()).toBe(MSG_APPLIED);
    expect(message()).toBe("Applied to camera. Capture remains stopped.");
    expect(unsaved()).toBeNull();
    expect(log.join("\n")).toContain(`Overview script applied from ${PROFILE_SCRIPT}`);
  });

  it("does not stop a camera that is stopped", async () => {
    await render();
    calls.length = 0;
    await click("Apply to Camera");
    expect(calls).toEqual([`save ${PROFILE_SCRIPT}`, `apply overview ${PROFILE_SCRIPT}`, "refresh state"]);
    expect(running).toEqual([]);
  });

  it("says what the camera refused", async () => {
    bridgeMock.applyCameraScriptWithRole.mockResolvedValue({ ok: false, message: "script error at line 3" });
    await render();
    await click("Apply to Camera");
    expect(message()).toBe("Failed to apply script: script error at line 3");
    expect(host.querySelector('[role="alert"]')).not.toBeNull();
    expect(log.join("\n")).toContain("Overview script not applied: script error at line 3");
  });

  it("applies only to an eGrabber hardware camera, and does not stop any other camera first", async () => {
    bridgeMock.fetchCameraSelection.mockImplementation(async () => selection({ mode: 1, running: true, label: "" }));   // the mock camera
    await render();
    await click("Apply to Camera");
    expect(message()).toBe("Failed to apply script: No hardware camera selected");
    expect(bridgeMock.stopCapture).not.toHaveBeenCalled();
    expect(bridgeMock.applyCameraScriptWithRole).not.toHaveBeenCalled();
  });

  it("does not apply when the camera cannot be stopped", async () => {
    bridgeMock.fetchCameraSelection.mockImplementation(async () => selection({ running: true }));
    bridgeMock.stopCapture.mockResolvedValue({ ok: false, message: "device busy" });
    await render();
    await click("Apply to Camera");
    expect(message()).toBe("Failed to apply script: the camera could not be stopped: device busy");
    expect(bridgeMock.applyCameraScriptWithRole).not.toHaveBeenCalled();
  });

  it("does not apply when the save fails, with Qt's 'Failed to save script'", async () => {
    await render();
    await type("x");
    files.get(PROFILE_SCRIPT)!.rev++;
    await click("Apply to Camera");
    expect(message()).toBe("Failed to save script: Camera file changed on disk; reload and reconcile your draft");
    expect(bridgeMock.fetchCameraSelection).not.toHaveBeenCalled();
    expect(bridgeMock.applyCameraScriptWithRole).not.toHaveBeenCalled();
  });

  it("is off during an experiment and while a raw recording runs, with the reason", async () => {
    await render({ expActive: true });
    expect(button("Apply to Camera").disabled).toBe(true);
    expect(button("Apply to Camera").title).toContain("finalize the experiment");
    await render({ expActive: false, recording: true });
    expect(button("Apply to Camera").disabled).toBe(true);
    expect(button("Apply to Camera").title).toContain("raw recording");
    await render({ recording: false });
    expect(button("Apply to Camera").disabled).toBe(false);
    expect(bridgeMock.applyCameraScriptWithRole).not.toHaveBeenCalled();
  });
});

describe("Browse... and Clear (CONNECT-059)", () => {
  beforeEach(() => { seed(PROFILE_SCRIPT, "// default\n"); seed("/usb/overview.js", "// external\n"); });

  it("loads the chosen file, keeps the choice across reloads and is what the stage change applies", async () => {
    dialogs.open.mockResolvedValue("/usb/overview.js");
    await render();
    await click("Browse...");
    expect(dialogs.open).toHaveBeenCalledWith(expect.objectContaining({ multiple: false, directory: false, title: "Select Camera script (overviewConfig.js)", defaultPath: PROFILE_SCRIPT }));
    expect(shownPath()).toBe("/usb/overview.js");
    expect(area().value).toBe("// external\n");
    expect(storage.get(OVERVIEW_SCRIPT_KEY)).toBe("/usb/overview.js");
    expect(model.path).toBe("/usb/overview.js");
    expect(model.defaultPath).toBe(PROFILE_SCRIPT);
    // a reload of the page: a fresh mount reads the stored path
    await act(async () => root.unmount());
    root = createRoot(host);
    await render();
    expect(shownPath()).toBe("/usb/overview.js");
    expect(area().value).toBe("// external\n");
    expect(files.get(PROFILE_SCRIPT)?.text).toBe("// default\n"); // the default is not touched
  });

  it("does nothing when the dialog is cancelled, and asks before it discards edits", async () => {
    dialogs.open.mockResolvedValue(null);
    await render();
    await type("edited");
    await click("Browse...");
    expect(storage.has(OVERVIEW_SCRIPT_KEY)).toBe(false);
    expect(area().value).toBe("edited");
    dialogs.confirm.mockResolvedValueOnce(false);
    dialogs.open.mockClear();
    await click("Browse...");
    expect(dialogs.confirm).toHaveBeenCalled();
    expect(dialogs.open).not.toHaveBeenCalled();
  });

  it("remembers a chosen file that cannot be loaded now, says why, and does not make a file for it", async () => {
    dialogs.open.mockResolvedValue("/usb/gone.js");
    await render();
    await click("Browse...");
    expect(message()).toBe("Failed to load: No such file or directory (os error 2)");
    expect(files.has("/usb/gone.js")).toBe(false);
    expect(storage.get(OVERVIEW_SCRIPT_KEY)).toBe("/usb/gone.js");
  });

  it("Clear forgets the path and asks Qt's question; Yes reloads the default file", async () => {
    storage.set(OVERVIEW_SCRIPT_KEY, "/usb/overview.js");
    await render();
    expect(area().value).toBe("// external\n");
    await click("Clear");
    expect(storage.has(OVERVIEW_SCRIPT_KEY)).toBe(false);
    expect(dialogs.ask).toHaveBeenCalledWith(MSG_CLEARED_QUESTION, expect.objectContaining({ title: "Camera Script Path Cleared" }));
    expect(shownPath()).toBe(PROFILE_SCRIPT);
    expect(area().value).toBe("// default\n");
    expect(model.path).toBe(PROFILE_SCRIPT);
    expect(unsaved()).toBeNull();
  });

  it("Clear then No keeps the text aimed at the default file: unsaved, and Save writes it there", async () => {
    storage.set(OVERVIEW_SCRIPT_KEY, "/usb/overview.js");
    dialogs.ask.mockResolvedValue(false);
    await render();
    await click("Clear");
    expect(storage.has(OVERVIEW_SCRIPT_KEY)).toBe(false);
    expect(shownPath()).toBe(PROFILE_SCRIPT);
    expect(area().value).toBe("// external\n");
    expect(unsaved()).not.toBeNull();
    expect(model.path).toBe(PROFILE_SCRIPT);
    await click("Save");
    expect(files.get(PROFILE_SCRIPT)?.text).toBe("// external\n");
    expect(files.get("/usb/overview.js")?.text).toBe("// external\n");
    expect(message()).toBe("Saved.");
  });
});

describe("who sees the editor (CONNECT-061)", () => {
  it("is not shown for a MindVision camera, and nothing is loaded or created for it", async () => {
    await render({ selection: selection({ mode: 3, mindvision_index: 0, mindvision_config_path: "C:\\p\\camera.json" }) });
    expect(host.innerHTML).toBe("");
    expect(invokeMock).not.toHaveBeenCalled();
    expect(model.visible).toBe(false);
    expect(host.textContent).not.toContain("JSON editor below");
  });

  it("comes back, loaded, when the camera is another one", async () => {
    seed(PROFILE_SCRIPT, "// back\n");
    await render({ selection: selection({ mode: 3 }) });
    expect(host.innerHTML).toBe("");
    await render({ selection: selection({ mode: 2 }) });
    expect(area().value).toBe("// back\n");
  });

  it("is not shown where the platform has no EGrabber scripts (the PZ7035)", async () => {
    await render({ eGrabberScripts: false });
    expect(host.innerHTML).toBe("");
    expect(bridgeMock.appPaths).not.toHaveBeenCalled();
  });

  it("is shown for the mock camera and before a camera is chosen, so the file can be prepared", async () => {
    await render({ selection: selection({ mode: 1, label: "" }) });
    expect(area()).not.toBeNull();
    await render({ selection: null });
    expect(area()).not.toBeNull();
  });
});
