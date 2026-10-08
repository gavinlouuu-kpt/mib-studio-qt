import { describe, expect, it, vi } from "vitest";

vi.mock("@tauri-apps/plugin-dialog", () => ({}));
vi.mock("@tauri-apps/plugin-opener", () => ({}));
vi.mock("./index", () => ({ isRemote: true }));

import { resolveRemoteDefault } from "./dialogs";

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
