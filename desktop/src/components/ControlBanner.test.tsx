// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

const invoke = vi.fn();
let state: { clientId?: number; controllerId?: number | null } = {};
const listeners = new Set<(s: typeof state) => void>();
vi.mock("../transport/index", () => ({
  transport: {
    kind: "ws",
    invoke: (...args: unknown[]) => invoke(...args),
    session: { get: () => state, subscribe: (l: (s: typeof state) => void) => { listeners.add(l); return () => listeners.delete(l); } },
  },
}));
import { ControlBanner } from "./ControlBanner";

let host: HTMLDivElement, root: Root;
const setState = async (next: typeof state) => { state = next; await act(async () => { listeners.forEach((l) => l(state)); }); };
beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  invoke.mockReset(); state = {}; listeners.clear();
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

describe("take-control banner (#501)", () => {
  it("is silent for the controller and before the server has said who is who", async () => {
    await act(async () => root.render(<ControlBanner />));
    expect(host.textContent).toBe("");
    await setState({ clientId: 7, controllerId: 7 });
    expect(host.textContent).toBe("");
    await setState({ clientId: 7, controllerId: null });
    expect(host.textContent).toBe("");
  });

  it("tells a viewer another client controls the instrument and claims control on request", async () => {
    invoke.mockResolvedValue({ controller_id: 7 });
    await act(async () => root.render(<ControlBanner />));
    await setState({ clientId: 7, controllerId: 3 });
    expect(host.textContent).toContain("Another client controls this instrument");
    await act(async () => host.querySelector("button")!.click());
    expect(invoke).toHaveBeenCalledWith("take_control");
    await setState({ clientId: 7, controllerId: 7 }); // the server broadcasts the change
    expect(host.textContent).toBe("");
  });

  it("shows why a claim failed", async () => {
    invoke.mockRejectedValue("TRANSPORT_LOST");
    await act(async () => root.render(<ControlBanner />));
    await setState({ clientId: 7, controllerId: 3 });
    await act(async () => host.querySelector("button")!.click());
    expect(host.querySelector('[role="alert"]')!.textContent).toContain("TRANSPORT_LOST");
  });
});
