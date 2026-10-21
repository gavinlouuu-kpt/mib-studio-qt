import { afterEach, describe, expect, it, vi } from "vitest";

const transport = vi.hoisted(() => ({ invoke: vi.fn() }));
vi.mock("@tauri-apps/plugin-dialog", () => ({}));
vi.mock("@tauri-apps/plugin-opener", () => ({}));
vi.mock("./index", () => ({ isRemote: true, invoke: transport.invoke }));

import { remoteSavePromptText, resolveRemoteDefault, save, setRemoteDefaultDir } from "./dialogs";

afterEach(() => { vi.unstubAllGlobals(); transport.invoke.mockReset(); setRemoteDefaultDir(""); });

describe("remote default paths (#651 G8)", () => {
  it("puts a relative default under the instrument's data directory", () => {
    expect(resolveRemoteDefault("/var/lib/yofo-studio", "experiment.h5")).toBe("/var/lib/yofo-studio/experiment.h5");
    expect(resolveRemoteDefault("/var/lib/yofo-studio/", "clip.h5")).toBe("/var/lib/yofo-studio/clip.h5");
  });
  it("leaves absolute defaults, empty defaults and an unknown data directory alone", () => {
    expect(resolveRemoteDefault("/var/lib/yofo-studio", "/mnt/ssd/run.h5")).toBe("/mnt/ssd/run.h5");
    expect(resolveRemoteDefault("/var/lib/yofo-studio", "C:\\data\\run.h5")).toBe("C:\\data\\run.h5");
    expect(resolveRemoteDefault("/var/lib/yofo-studio", "")).toBe("");
    expect(resolveRemoteDefault("/var/lib/yofo-studio", undefined)).toBe("");
    expect(resolveRemoteDefault("", "experiment.h5")).toBe("experiment.h5");
  });
});

// #762: the browser Start prompt (no native dialog) defaults to <storage.path>/<suggested name>, with
// storage.path from fetch_instrument_status, and says where the file goes, on any server (a PC with a mock
// camera too: the PZ7035 status poll that used to set the directory never runs there).
describe("the browser save prompt (#762)", () => {
  it("defaults to <storage.path>/experiment.h5 from fetch_instrument_status and says where the file goes", async () => {
    transport.invoke.mockResolvedValue({ available: false, error: "not a PZ7035 instrument", storage: { path: "/tmp/workspace-walk/data" } });
    const prompt = vi.fn(() => "/tmp/workspace-walk/data/experiment.h5");
    vi.stubGlobal("window", { prompt });
    await expect(save({ title: "Save Experiment Data", defaultPath: "experiment.h5" })).resolves.toBe("/tmp/workspace-walk/data/experiment.h5");
    expect(transport.invoke).toHaveBeenCalledWith("fetch_instrument_status");
    expect(prompt).toHaveBeenCalledWith(
      "Save Experiment Data\nThe file is saved on the instrument, in /tmp/workspace-walk/data. A relative path is saved there too.",
      "/tmp/workspace-walk/data/experiment.h5");
  });
  it("uses the storage path of the moment, not one an earlier poll left", async () => {
    setRemoteDefaultDir("/old/data");
    transport.invoke.mockResolvedValue({ available: true, storage: { path: "/var/lib/yofo-studio" } });
    const prompt = vi.fn(() => null);
    vi.stubGlobal("window", { prompt });
    await expect(save({ title: "Recording output", defaultPath: "clip.h5" })).resolves.toBeNull();
    expect(prompt).toHaveBeenCalledWith(expect.stringContaining("in /var/lib/yofo-studio."), "/var/lib/yofo-studio/clip.h5");
  });
  it("falls back to the last known directory when the status cannot be fetched, and keeps an absolute default", async () => {
    setRemoteDefaultDir("/var/lib/yofo-studio");
    transport.invoke.mockRejectedValue(new Error("offline"));
    const prompt = vi.fn(() => " run.h5 ");
    vi.stubGlobal("window", { prompt });
    await expect(save({ title: "Save Experiment Data", defaultPath: "experiment.h5" })).resolves.toBe("run.h5");
    expect(prompt).toHaveBeenLastCalledWith(expect.stringContaining("in /var/lib/yofo-studio."), "/var/lib/yofo-studio/experiment.h5");
    await save({ title: "Save Experiment Data", defaultPath: "/mnt/ssd/run.h5" });
    expect(prompt).toHaveBeenLastCalledWith(expect.any(String), "/mnt/ssd/run.h5");
  });
  it("with no directory known, keeps the suggested name and still says where a relative path goes", async () => {
    transport.invoke.mockResolvedValue({ available: false });
    const prompt = vi.fn(() => "experiment.h5");
    vi.stubGlobal("window", { prompt });
    await save({ title: "Save Experiment Data", defaultPath: "experiment.h5" });
    expect(prompt).toHaveBeenCalledWith(remoteSavePromptText("Save Experiment Data", ""), "experiment.h5");
    expect(remoteSavePromptText("Save Experiment Data", "")).toContain("A relative path is saved in its data directory.");
  });
});
