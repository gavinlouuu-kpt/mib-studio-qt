import { afterEach, describe, expect, it, vi } from "vitest";

// transport/dialogs `message` (epic #866: the Start Experiment refusals are modal, as in the Qt app): the plugin's
// message in the desktop shell, window.alert in a browser, and never an error for the caller.
const plugin = vi.hoisted(() => ({ message: vi.fn(async () => "Ok") }));
const remote = vi.hoisted(() => ({ isRemote: false }));
vi.mock("@tauri-apps/plugin-dialog", () => plugin);
vi.mock("@tauri-apps/plugin-opener", () => ({}));
vi.mock("./index", () => ({ get isRemote() { return remote.isRemote; } }));

import { message } from "./dialogs";

afterEach(() => { vi.unstubAllGlobals(); plugin.message.mockReset(); plugin.message.mockResolvedValue("Ok"); });

describe("dialogs.message", () => {
  it("passes the text and options to the plugin unchanged when local", async () => {
    remote.isRemote = false;
    const options = { title: "Start Experiment", kind: "warning" as const };
    await message("Blocked — camera.rate", options);
    expect(plugin.message).toHaveBeenCalledWith("Blocked — camera.rate", options);
  });
  it("alerts with the title first in a browser", async () => {
    remote.isRemote = true;
    const alert = vi.fn();
    vi.stubGlobal("window", { alert });
    await message("Failed to prepare the file", { title: "Error", kind: "error" });
    expect(alert).toHaveBeenCalledWith("Error\n\nFailed to prepare the file");
    expect(plugin.message).not.toHaveBeenCalled();
    await message("plain");
    expect(alert).toHaveBeenLastCalledWith("plain");
  });
  it("never throws when the dialog cannot be shown", async () => {
    remote.isRemote = false;
    plugin.message.mockRejectedValue(new Error("no window"));
    await expect(message("text", "Title")).resolves.toBeUndefined();
  });
});
