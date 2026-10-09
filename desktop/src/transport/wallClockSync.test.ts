import { describe, expect, it, vi } from "vitest";
import { createWallClockSyncer, isController } from "./wallClockSync";

describe("wall clock sync (G14)", () => {
  it("is the controller only when the server says so", () => {
    expect(isController({})).toBe(false);
    expect(isController({ clientId: 7, controllerId: 3 })).toBe(false);
    expect(isController({ clientId: 7, controllerId: null })).toBe(false);
    expect(isController({ clientId: 7, controllerId: 7 })).toBe(true);
  });

  it("sends the client's time once when it becomes the controller, then on each tick", () => {
    const invoke = vi.fn().mockResolvedValue({ ok: true });
    let t = 1_790_000_000_000.4;
    const s = createWallClockSyncer(invoke, () => t);
    s.onSession({ clientId: 7, controllerId: 3 }); // a viewer sends nothing
    s.tick();
    expect(invoke).not.toHaveBeenCalled();
    s.onSession({ clientId: 7, controllerId: 7 });
    expect(invoke).toHaveBeenCalledTimes(1);
    expect(invoke).toHaveBeenLastCalledWith("sync_wall_clock", { unixMs: 1_790_000_000_000 });
    s.onSession({ clientId: 7, controllerId: 7 }); // the same role again: no repeat
    expect(invoke).toHaveBeenCalledTimes(1);
    t += 600_000;
    s.tick();
    expect(invoke).toHaveBeenCalledTimes(2);
    s.onSession({ clientId: 7, controllerId: 9 }); // control lost
    s.tick();
    expect(invoke).toHaveBeenCalledTimes(2);
    s.onSession({ clientId: 7, controllerId: 7 }); // and regained: sent again
    expect(invoke).toHaveBeenCalledTimes(3);
  });

  it("swallows a failed sync", async () => {
    const invoke = vi.fn().mockRejectedValue(new Error("VIEWER_ONLY"));
    const s = createWallClockSyncer(invoke, () => 1_790_000_000_000);
    s.onSession({ clientId: 1, controllerId: 1 });
    await Promise.resolve();
    expect(invoke).toHaveBeenCalledTimes(1);
  });
});
