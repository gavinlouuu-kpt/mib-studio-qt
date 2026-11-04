import { afterEach, describe, expect, it, vi } from "vitest";

// transport/dialogs `choose` (#858): Qt's QMessageBox with custom buttons. The plugin's message with custom buttons in the
// desktop shell (it resolves to the clicked label), window.confirm per answer in a browser, and "cancel" for anything else.
const plugin = vi.hoisted(() => ({ message: vi.fn(async (): Promise<string> => "Ok") }));
const remote = vi.hoisted(() => ({ isRemote: false }));
vi.mock("@tauri-apps/plugin-dialog", () => plugin);
vi.mock("@tauri-apps/plugin-opener", () => ({}));
vi.mock("./index", () => ({ get isRemote() { return remote.isRemote; } }));

import { choiceOf, choose } from "./dialogs";

const three = { title: "Start Experiment", kind: "warning" as const, yes: "Switch to Every Frame", no: "Continue with Latest Frame", cancel: "Cancel" };
const two = { title: "Start Experiment", yes: "Continue with Latest Frame", cancel: "Cancel" };

afterEach(() => { plugin.message.mockReset(); remote.isRemote = false; vi.unstubAllGlobals(); });

describe("choose in the desktop shell", () => {
  it("shows three custom buttons and maps the clicked label", async () => {
    for (const [label, want] of [["Switch to Every Frame", "yes"], ["Continue with Latest Frame", "no"], ["Cancel", "cancel"]] as const) {
      plugin.message.mockResolvedValueOnce(label);
      expect(await choose("text", three)).toBe(want);
    }
    expect(plugin.message).toHaveBeenCalledWith("text", {
      title: "Start Experiment", kind: "warning", buttons: { yes: "Switch to Every Frame", no: "Continue with Latest Frame", cancel: "Cancel" },
    });
  });
  it("shows two buttons (ok / cancel) when there is no second answer", async () => {
    plugin.message.mockResolvedValueOnce("Continue with Latest Frame");
    expect(await choose("text", two)).toBe("yes");
    expect(plugin.message).toHaveBeenCalledWith("text", { title: "Start Experiment", kind: undefined, buttons: { ok: "Continue with Latest Frame", cancel: "Cancel" } });
  });
  it("a closed dialog or an unknown answer is cancel: never an answer the operator did not give", async () => {
    plugin.message.mockResolvedValueOnce("Cancel");
    expect(await choose("text", two)).toBe("cancel");
    plugin.message.mockResolvedValueOnce("something else");
    expect(await choose("text", three)).toBe("cancel");
  });
  it("a dialog that cannot be shown rejects, so the caller can say so", async () => {
    plugin.message.mockRejectedValueOnce(new Error("dialog:allow-message denied"));
    await expect(choose("text", three)).rejects.toThrow("dialog:allow-message denied");
  });
  it("accepts the default button names too", () => {
    expect(choiceOf("Yes", three)).toBe("yes");
    expect(choiceOf("Ok", two)).toBe("yes");
    expect(choiceOf("No", three)).toBe("no");
    expect(choiceOf("No", two)).toBe("cancel");
    expect(choiceOf(null, three)).toBe("cancel");
  });
});

describe("choose in a browser (remote instrument)", () => {
  it("asks one confirm per answer, in order", async () => {
    remote.isRemote = true;
    const confirm = vi.fn().mockReturnValueOnce(false).mockReturnValueOnce(true);
    vi.stubGlobal("window", { confirm });
    expect(await choose("text", three)).toBe("no");
    expect(confirm).toHaveBeenNthCalledWith(1, "Start Experiment\n\ntext\n\nOK: Switch to Every Frame\nCancel: Continue with Latest Frame");
    expect(confirm).toHaveBeenNthCalledWith(2, "Start Experiment\n\ntext\n\nOK: Continue with Latest Frame\nCancel: Cancel");
    expect(plugin.message).not.toHaveBeenCalled();
  });
  it("two answers need one confirm; declining it is cancel", async () => {
    remote.isRemote = true;
    const confirm = vi.fn().mockReturnValue(false);
    vi.stubGlobal("window", { confirm });
    expect(await choose("text", two)).toBe("cancel");
    expect(confirm).toHaveBeenCalledTimes(1);
  });
});
