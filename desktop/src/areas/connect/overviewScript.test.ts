// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { CameraSelection } from "../../bridge";

const invokeMock = vi.hoisted(() => vi.fn());
vi.mock("../../transport", () => ({ invoke: invokeMock }));
import {
  createOverviewDefault, differsFromFile, ensureOverviewDefault, isHardwareCamera, loadOverviewScript, MSG_APPLIED, MSG_CLEARED_QUESTION,
  MSG_SAVED, MSG_UNSAVED, OVERVIEW_SCRIPT_KEY, overviewApplyBlock, overviewDefaultPath, overviewScriptPath, overviewScriptVisible,
  readExternalPath, withLineEndings, writeExternalPath,
} from "./overviewScript";

const sel = (over: Partial<CameraSelection> = {}) => ({ valid: true, configured: true, mode: 2, ...over }) as CameraSelection;

describe("Qt's texts", () => {
  it("are kept word for word", () => {
    expect(MSG_SAVED).toBe("Saved.");
    expect(MSG_APPLIED).toBe("Applied to camera. Capture remains stopped.");
    expect(MSG_UNSAVED).toBe("Unsaved changes – click Save to apply.");
    expect(MSG_CLEARED_QUESTION).toBe("External Camera script path cleared.\nReset from default include path now?\n\nNote: Save to apply any changes.");
  });
});

describe("the Overview script's path (CONNECT-056, Qt currentJsPath / defaultJsPath)", () => {
  it("defaults to overviewConfig.js beside the active profile's config.json, with the path's own separators", () => {
    expect(overviewDefaultPath({ profileConfigPath: "/data/profiles/lab/config.json", appConfigDir: "/home/u/.config/app" })).toBe("/data/profiles/lab/overviewConfig.js");
    expect(overviewDefaultPath({ profileConfigPath: "C:\\MIBTest\\include\\config.json", appConfigDir: "C:\\Users\\u\\AppData\\Roaming\\app" })).toBe("C:\\MIBTest\\include\\overviewConfig.js");
  });
  it("is the app config folder's file when no profile is applied, and empty while that is not known", () => {
    expect(overviewDefaultPath({ profileConfigPath: "", appConfigDir: "/home/u/.config/app" })).toBe("/home/u/.config/app/overviewConfig.js");
    expect(overviewDefaultPath({ profileConfigPath: "", appConfigDir: "" })).toBe("");
  });
  it("is the external file when one is set, trimmed, else the default", () => {
    expect(overviewScriptPath("/usb/overview.js", "/p/overviewConfig.js")).toBe("/usb/overview.js");
    expect(overviewScriptPath("  /usb/overview.js \n", "/p/overviewConfig.js")).toBe("/usb/overview.js");
    expect(overviewScriptPath("", "/p/overviewConfig.js")).toBe("/p/overviewConfig.js");
    expect(overviewScriptPath("   ", "/p/overviewConfig.js")).toBe("/p/overviewConfig.js");
  });
});

describe("who sees the script area (CONNECT-061)", () => {
  it("hides it for a MindVision camera and where the platform has no EGrabber scripts, and shows it otherwise", () => {
    expect(overviewScriptVisible({ eGrabberScripts: true, selection: sel({ mode: 3 }) })).toBe(false);
    expect(overviewScriptVisible({ eGrabberScripts: false, selection: sel() })).toBe(false);
    expect(overviewScriptVisible({ eGrabberScripts: true, selection: sel({ mode: 2 }) })).toBe(true);
    expect(overviewScriptVisible({ eGrabberScripts: true, selection: sel({ mode: 1 }) })).toBe(true);   // the mock
    expect(overviewScriptVisible({ eGrabberScripts: true, selection: sel({ mode: 0, configured: false }) })).toBe(true); // nothing selected yet
    expect(overviewScriptVisible({ eGrabberScripts: true, selection: null })).toBe(true);
  });
});

describe("Apply to Camera guards (CONNECT-058)", () => {
  it("blocks while the backend is not ready, an experiment runs or a raw recording is on", () => {
    expect(overviewApplyBlock({ ready: true, expActive: false, recording: false })).toBe("");
    expect(overviewApplyBlock({ ready: false, expActive: false, recording: false })).toContain("not initialized");
    expect(overviewApplyBlock({ ready: true, expActive: true, recording: false })).toContain("finalize the experiment");
    expect(overviewApplyBlock({ ready: true, expActive: false, recording: true })).toContain("raw recording");
  });
  it("applies only to an eGrabber hardware camera", () => {
    expect(isHardwareCamera(sel())).toBe(true);
    expect(isHardwareCamera(sel({ mode: 1 }))).toBe(false);
    expect(isHardwareCamera(sel({ mode: 3 }))).toBe(false);
    expect(isHardwareCamera(sel({ configured: false }))).toBe(false);
    expect(isHardwareCamera(sel({ valid: false }))).toBe(false);
  });
});

describe("line endings", () => {
  it("keeps a CRLF script CRLF after the textarea reports LF, and leaves other scripts as typed", () => {
    expect(withLineEndings("a\nb\n", "x\r\ny\r\n")).toBe("a\r\nb\r\n");
    expect(withLineEndings("a\nb\n", "x\ny\n")).toBe("a\nb\n");
    expect(withLineEndings("a\nb\n", "x\r\ny\n")).toBe("a\nb\n");     // mixed: as typed
    expect(withLineEndings("a\r\nb\r\n", "x\r\ny\r\n")).toBe("a\r\nb\r\n");
    expect(withLineEndings("a\nb", "single line")).toBe("a\nb");
  });
  it("does not count the textarea's line-ending normalization as an edit", () => {
    expect(differsFromFile("a\nb\n", "a\r\nb\r\n")).toBe(false);
    expect(differsFromFile("a\nc\n", "a\r\nb\r\n")).toBe(true);
  });
});

// jsdom under this Node has no usable localStorage: a Map-backed stand-in, as profiles.test does.
const storage = new Map<string, string>();
const store = {
  getItem: (k: string) => storage.get(k) ?? null, setItem: (k: string, v: string) => { storage.set(k, v); },
  removeItem: (k: string) => { storage.delete(k); }, clear: () => storage.clear(),
};

describe("the external path kept across reloads (CONNECT-059, Qt Config/ExternalOverviewScriptPath)", () => {
  beforeEach(() => { Object.defineProperty(window, "localStorage", { configurable: true, value: store }); storage.clear(); });
  afterEach(() => vi.restoreAllMocks());
  it("is stored under mib.overview.script-path and read back, trimmed", () => {
    expect(OVERVIEW_SCRIPT_KEY).toBe("mib.overview.script-path");
    expect(readExternalPath()).toBe("");
    writeExternalPath("/usb/overview.js");
    expect(window.localStorage.getItem("mib.overview.script-path")).toBe("/usb/overview.js");
    expect(readExternalPath()).toBe("/usb/overview.js");
    window.localStorage.setItem(OVERVIEW_SCRIPT_KEY, "  /usb/x.js  ");
    expect(readExternalPath()).toBe("/usb/x.js");
  });
  it("is removed by an empty path", () => {
    writeExternalPath("/usb/overview.js");
    writeExternalPath("");
    expect(window.localStorage.getItem(OVERVIEW_SCRIPT_KEY)).toBeNull();
    expect(readExternalPath()).toBe("");
  });
  it("survives storage that throws (a private window, blocked site data)", () => {
    vi.spyOn(store, "getItem").mockImplementation(() => { throw new Error("blocked"); });
    vi.spyOn(store, "setItem").mockImplementation(() => { throw new Error("blocked"); });
    vi.spyOn(store, "removeItem").mockImplementation(() => { throw new Error("blocked"); });
    expect(readExternalPath()).toBe("");
    expect(() => writeExternalPath("/usb/overview.js")).not.toThrow();
    expect(() => writeExternalPath("")).not.toThrow();
  });
});

describe("loading the script through the camera_document command", () => {
  beforeEach(() => { invokeMock.mockReset(); });
  const BUNDLED = "// bundled overviewConfig.js\n";

  it("reads an existing file as kind 'overview'", async () => {
    invokeMock.mockResolvedValueOnce({ path: "/p/overviewConfig.js", text: "// mine", revision: "r1" });
    const r = await loadOverviewScript("/p/overviewConfig.js", true);
    expect(r).toEqual({ doc: { path: "/p/overviewConfig.js", text: "// mine", revision: "r1" }, created: false });
    expect(invokeMock).toHaveBeenCalledWith("camera_document", { action: "read", path: "/p/overviewConfig.js", kind: "overview", baseline: "", text: "" });
  });

  it("makes the default file from the bundled default when it is missing (Qt ensureDefaultsFile)", async () => {
    invokeMock.mockImplementation(async (_c: string, a: { action: string; path: string; text: string }) => {
      if (a.action === "read") throw new Error("No such file or directory (os error 2)");
      if (a.action === "default") return { path: "", text: BUNDLED, revision: "" };
      return { path: a.path, text: a.text, revision: "r1" };
    });
    const r = await loadOverviewScript("/p/overviewConfig.js", true);
    expect(r.created).toBe(true);
    expect(r.doc.text).toBe(BUNDLED);
    expect(invokeMock).toHaveBeenCalledWith("camera_document", { action: "create", path: "/p/overviewConfig.js", kind: "overview", baseline: "", text: BUNDLED });
  });

  it("does not make a file for an external path: the read error is the answer", async () => {
    invokeMock.mockRejectedValue(new Error("No such file or directory (os error 2)"));
    await expect(loadOverviewScript("/usb/overview.js", false)).rejects.toThrow("No such file or directory");
    expect(invokeMock).toHaveBeenCalledTimes(1);
  });

  it("reports the read error, not the create error, for a default file that exists but cannot be read", async () => {
    invokeMock.mockImplementation(async (_c: string, a: { action: string }) => {
      if (a.action === "read") throw new Error("Permission denied (os error 13)");
      if (a.action === "default") return { path: "", text: BUNDLED, revision: "" };
      throw new Error("File exists (os error 17)");
    });
    await expect(loadOverviewScript("/p/overviewConfig.js", true)).rejects.toThrow("Permission denied");
  });

  it("createOverviewDefault and ensureOverviewDefault use the bundled text and never replace a file", async () => {
    invokeMock.mockImplementation(async (_c: string, a: { action: string; path: string; text: string }) => {
      if (a.action === "default") return { path: "", text: BUNDLED, revision: "" };
      return { path: a.path, text: a.text, revision: "r1" };
    });
    expect((await createOverviewDefault("/p/overviewConfig.js")).text).toBe(BUNDLED);
    expect(await ensureOverviewDefault("/p/overviewConfig.js")).toBe(true);
    invokeMock.mockImplementation(async (_c: string, a: { action: string }) => {
      if (a.action === "default") return { path: "", text: BUNDLED, revision: "" };
      throw new Error("File exists (os error 17)");
    });
    expect(await ensureOverviewDefault("/p/overviewConfig.js")).toBe(false);
  });
});
