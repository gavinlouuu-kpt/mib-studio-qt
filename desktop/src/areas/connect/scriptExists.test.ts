// @vitest-environment jsdom
// #892 review: the profile-folder script probe on the desktop and on a remote (browser) client.
import { beforeEach, describe, expect, it, vi } from "vitest";

const invoke = vi.fn();
vi.mock("../../transport", () => ({ invoke: (...args: unknown[]) => invoke(...args), isRemote: false }));
vi.mock("../../bridge", () => ({ bridge: {} }));

import { scriptExists } from "./useConnect";

describe("scriptExists (#892 review: remote clients)", () => {
  beforeEach(() => invoke.mockReset());

  it("on the desktop reads the file through camera_document", async () => {
    invoke.mockResolvedValueOnce({ text: "" });
    expect(await scriptExists("/p/lab/egrabberConfig.js")).toBe(true);
    expect(invoke).toHaveBeenCalledWith("camera_document", { action: "read", path: "/p/lab/egrabberConfig.js", kind: "js", baseline: "", text: "" });
    invoke.mockRejectedValueOnce(new Error("not found"));
    expect(await scriptExists("/p/lab/missing.js")).toBe(false);
  });

  it("on a remote client answers false without asking the server, which refuses .js reads", async () => {
    expect(await scriptExists("/p/lab/egrabberConfig.js", true)).toBe(false);
    expect(invoke).not.toHaveBeenCalled();
  });
});
