// @vitest-environment jsdom
// Prepare's readiness read: a reply that lands after polling stopped (a run started, the view
// left) is dropped, so it never restores the cleared snapshot (Codex round 1 on #782).
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { invoke } from "@tauri-apps/api/core";
import { JSON_TRANSPORT } from "../bridgeContract";
import { usePrepareReadiness, type PrepareReadiness } from "./usePrepareReadiness";

vi.mock("@tauri-apps/api/core", () => ({ invoke: vi.fn() }));

let host: HTMLDivElement, root: Root;
let latest: PrepareReadiness | null = null;
function Probe({ active }: { active: boolean }) {
  latest = usePrepareReadiness(active);
  return null;
}
const render = (active: boolean) => act(async () => root.render(<Probe active={active} />));
const reply = { transport_version: JSON_TRANSPORT.version, valid: true, ready: false, generation: "5", gates: [] };

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  vi.useFakeTimers();
  vi.resetAllMocks();
  latest = null;
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => {
  await act(async () => root.unmount());
  host.remove();
  vi.useRealTimers();
});

it("a read in flight when polling stops is dropped: the cleared state stays cleared", async () => {
  let resolve: (v: unknown) => void = () => {};
  vi.mocked(invoke).mockImplementation(async (cmd: string) => {
    if (cmd !== "fetch_experiment_readiness") throw new Error(`unexpected ${cmd}`);
    return new Promise((r) => { resolve = r; });
  });
  await render(true);
  expect(vi.mocked(invoke)).toHaveBeenCalledTimes(1); // the read is pending
  await render(false); // a run starts or the view is left: polling stops, state cleared
  expect(latest!.snapshot).toBeNull();
  await act(async () => { resolve(reply); await vi.advanceTimersByTimeAsync(0); });
  expect(latest!.snapshot).toBeNull();
  expect(latest!.error).toBe("");
  expect(vi.mocked(invoke)).toHaveBeenCalledTimes(1); // nothing read again while inactive
});

it("a read that answers while polling is active is shown", async () => {
  vi.mocked(invoke).mockResolvedValue(reply);
  await render(true);
  await act(async () => { await vi.advanceTimersByTimeAsync(0); });
  expect(latest!.snapshot?.generation).toBe("5");
});
