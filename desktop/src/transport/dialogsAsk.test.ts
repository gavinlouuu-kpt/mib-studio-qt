import {afterEach, describe, expect, it, vi} from "vitest";

// transport/dialogs `ask` (#512 S9): the plugin's Yes / No question in the desktop shell,
// window.confirm in a browser.
const plugin = vi.hoisted(() => ({ask: vi.fn(async () => true)}));
const remote = vi.hoisted(() => ({isRemote: false}));
vi.mock("@tauri-apps/plugin-dialog", () => plugin);
vi.mock("@tauri-apps/plugin-opener", () => ({}));
vi.mock("./index", () => ({get isRemote() { return remote.isRemote; }}));

import {ask} from "./dialogs";

afterEach(() => { vi.unstubAllGlobals(); plugin.ask.mockClear(); });

describe("dialogs.ask", () => {
  it("passes the message and options to the plugin unchanged when local", async () => {
    remote.isRemote = false;
    const options = {title: "Replace core contour", kind: "warning" as const};
    await expect(ask("Replace it?", options)).resolves.toBe(true);
    expect(plugin.ask).toHaveBeenCalledWith("Replace it?", options);
    expect((plugin.ask.mock.calls[0] as unknown[])[1]).toBe(options);
  });

  it("asks with window.confirm in a browser", async () => {
    remote.isRemote = true;
    const confirm = vi.fn(() => false);
    vi.stubGlobal("window", {confirm});
    await expect(ask("Save this contour?", {title: "Save core contour", kind: "info"})).resolves.toBe(false);
    expect(confirm).toHaveBeenCalledWith("Save this contour?");
    expect(plugin.ask).not.toHaveBeenCalled();
  });
});
