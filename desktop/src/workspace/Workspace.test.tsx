// @vitest-environment jsdom
// The opt-in workspace and its Prepare view over the mocked transport (#713): Prepare reads the
// backend gates without a path and nothing else, renders Unknown as unknown, and stages only
// change what is shown.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { invoke } from "@tauri-apps/api/core";
import { JSON_TRANSPORT, READINESS_GATE_STATUSES as G } from "../bridgeContract";
import type { ReadinessGate } from "../eventAdapter";
import type { PreflightInput } from "../preflight";
import { PREPARE_READINESS_POLL_MS } from "./prepareModel";
import { Workspace, type WorkspaceProps } from "./Workspace";

vi.mock("@tauri-apps/api/core", () => ({ invoke: vi.fn() }));

const OFF = { valid: true, connected: false, identity: "" };
const INPUT: PreflightInput = {
  backendReady: true, cameraConfigured: true, cameraRunning: false, cameraIdentity: "Mock camera", cameraExpected: "",
  coreKnown: true, coreValid: true, corePinSatisfied: true, coreVersion: "2.1.0", requiredCoreVersion: "2.1.0",
  autofocus: OFF, samplePump: OFF, sheathPump: OFF, trigger: { valid: true, cameraAttached: false },
  storageKnown: false, storageWritable: false, storageFreeOk: false, storagePath: "",
};
const NO_PATH: ReadinessGate = { id: "storage.output", status: G.Unavailable, reason: "no output path chosen yet", remediation: "choose an HDF5 destination" };
let gates: ReadinessGate[] = [{ id: "camera.source", status: G.Pass, reason: "", remediation: "" }, NO_PATH];
const readinessReply = () => ({ transport_version: JSON_TRANSPORT.version, valid: true, ready: false, generation: "3", gates });

let host: HTMLDivElement, root: Root;
const onStage = vi.fn(), onExit = vi.fn(), onConfirm = vi.fn(), onOpenSetup = vi.fn(), onRefreshChecks = vi.fn();
const props = (over: Partial<WorkspaceProps> = {}): WorkspaceProps => ({
  brand: "MIB Studio", source: "Mock camera", method: "", stage: 0, onStage, onExit, serviceMode: false,
  preflightInput: INPUT, zStage: null, signature: "sig-now", confirmedFor: "", onConfirm, confirmLockedReason: "",
  experimentActive: false, onRefreshChecks, onOpenSetup, ...over,
});
const render = (over: Partial<WorkspaceProps> = {}) => act(async () => root.render(<Workspace {...props(over)} />));
const advance = (ms: number) => act(async () => { await vi.advanceTimersByTimeAsync(ms); });
const calls = () => vi.mocked(invoke).mock.calls.map(([cmd, args]) => ({ cmd, args }));
const button = (text: string) => [...host.querySelectorAll("button")].find((b) => b.textContent === text)!;
const row = (label: string, scope = "") => [...host.querySelectorAll(`${scope} dt`)].find((d) => d.textContent === label)?.nextElementSibling as HTMLElement | undefined;
const GATES = "[data-testid=workspace-gates]";

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  vi.useFakeTimers();
  vi.resetAllMocks();
  gates = [{ id: "camera.source", status: G.Pass, reason: "", remediation: "" }, NO_PATH];
  vi.mocked(invoke).mockImplementation(async (cmd: string) => {
    if (cmd === "fetch_experiment_readiness") return readinessReply();
    throw new Error(`unexpected command ${cmd}`);
  });
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => {
  await act(async () => root.unmount());
  host.remove();
  vi.useRealTimers();
});

describe("Prepare reads readiness without a path (#713)", () => {
  it("polls only fetch_experiment_readiness, always with an empty output path, so no probe file is written", async () => {
    await render();
    await advance(PREPARE_READINESS_POLL_MS * 5);
    const log = calls();
    expect(log.length).toBe(6); // the first read, then one per period
    for (const c of log) expect(c).toEqual({ cmd: "fetch_experiment_readiness", args: { outputPath: "" } });
  });

  it("Refresh and Read gates again read without a path too", async () => {
    await render();
    await act(async () => button("Read gates again").click());
    await act(async () => button("Refresh").click());
    await advance(0);
    expect(calls().every((c) => c.cmd === "fetch_experiment_readiness" && (c.args as { outputPath: string }).outputPath === "")).toBe(true);
    expect(onRefreshChecks).toHaveBeenCalledTimes(1);
  });

  it("stops polling on another stage and while a run is active", async () => {
    await render();
    await render({ stage: 1 });
    const before = calls().length;
    await advance(PREPARE_READINESS_POLL_MS * 4);
    expect(calls().length).toBe(before);
    await render({ stage: 0, experimentActive: true });
    await advance(PREPARE_READINESS_POLL_MS * 4);
    expect(calls().length).toBe(before);
    expect(host.textContent).toContain("Readiness paused · a run is active");
  });
});

describe("Prepare states", () => {
  it("shows the gates read-only, the destination as Unavailable and no blocker otherwise", async () => {
    await render();
    const chip = row("storage.output", GATES)!.querySelector(".mib-chip")!;
    expect(row("storage.output")!.textContent).toBe("Unavailable · no output path chosen yet"); // the destination card
    expect(chip.textContent).toBe("Unavailable · no output path chosen yet");
    expect(chip.getAttribute("data-tone")).toBe("unknown");
    expect(chip.getAttribute("title")).toBe("choose an HDF5 destination");
    const status = host.querySelector(".mib-shell__pills .mib-chip")!;
    expect(status.textContent).toBe("Readiness · No blockers · destination checked at Start");
    expect(status.getAttribute("data-tone")).toBe("info");
    expect(host.textContent).toContain("Unknown is never Pass. Unsupported never blocks.");
  });

  it("is Unknown before the first read, never a pass", async () => {
    vi.mocked(invoke).mockImplementation(() => new Promise(() => {})); // never answers
    await render();
    const status = host.querySelector(".mib-shell__pills .mib-chip")!;
    expect(status.textContent).toBe("Readiness · Unknown · readiness not read yet");
    expect(status.getAttribute("data-tone")).toBe("unknown");
  });

  it("names the blocking gates", async () => {
    gates = [{ id: "camera.source", status: G.Unavailable, reason: "no camera source configured", remediation: "" }, { id: "processing.core", status: G.Fail, reason: "pin", remediation: "" }, NO_PATH];
    await render();
    const status = host.querySelector(".mib-shell__pills .mib-chip")!;
    expect(status.textContent).toBe("Readiness · Start blocked · 2 gates");
    expect(status.getAttribute("data-tone")).toBe("blocked");
  });

  it("a failed read clears the gates instead of showing stale ones", async () => {
    await render();
    expect(row("camera.source", GATES)).toBeDefined();
    vi.mocked(invoke).mockRejectedValue(new Error("bridge gone"));
    await advance(PREPARE_READINESS_POLL_MS);
    expect(row("camera.source", GATES)).toBeUndefined();
    expect(host.querySelector(".mib-shell__pills .mib-chip")!.getAttribute("data-tone")).toBe("unknown");
  });

  it("renders a check not read yet as Unknown, not failed, and keeps Confirm disabled", async () => {
    await render({ preflightInput: { ...INPUT, coreKnown: false, coreValid: false } });
    const core = row("Processing core / trust · required")!;
    expect(core.textContent).toBe("Unknown · expected 2.1.0 · Processing-core status not read yet.");
    expect(core.className).toContain("mib-facts__value--unknown");
    expect(core.className).not.toContain("blocked");
    expect(host.textContent).toContain("Required checks: 1 of 2 ready · 1 unknown");
    expect(button("Confirm readiness").disabled).toBe(true);
    // Storage that nothing reported is unknown, not "not required".
    expect(row("Storage destination")!.textContent).toMatch(/^Unknown · /);
  });

  it("confirms only on the explicit press, and the confirmation sends nothing to the backend", async () => {
    await render();
    const before = calls().length;
    expect(button("Confirm readiness").disabled).toBe(false);
    await act(async () => button("Confirm readiness").click());
    expect(onConfirm).toHaveBeenCalledTimes(1);
    expect(calls().length).toBe(before);
    await render({ confirmedFor: "sig-now" });
    expect(row("Preflight confirmation")!.textContent).toBe("Confirmed for this setup");
    expect(button("Confirm readiness").disabled).toBe(true);
    await render({ confirmedFor: "sig-before" });
    expect(row("Preflight confirmation")!.textContent).toContain("Setup changed since the confirmation");
  });

  it("a required warning blocks Confirm only when its backend gate says Fail", async () => {
    await render({ preflightInput: { ...INPUT, cameraExpected: "Other camera" } });
    expect(button("Confirm readiness").disabled).toBe(false);
    gates = [{ id: "camera.source", status: G.Fail, reason: "simulated", remediation: "" }, NO_PATH];
    await advance(PREPARE_READINESS_POLL_MS);
    expect(button("Confirm readiness").disabled).toBe(true);
  });
});

describe("stages only navigate", () => {
  it("selecting a stage calls onStage and sends nothing but the Prepare reads", async () => {
    await render();
    const tabs = [...host.querySelectorAll<HTMLButtonElement>(".mib-stages__tab")];
    expect(tabs.map((t) => t.textContent)).toEqual(["01Prepare", "02Align", "03Experiment", "04Review"]);
    for (const t of tabs) await act(async () => t.click());
    expect(onStage.mock.calls.map(([i]) => i)).toEqual([0, 1, 2, 3]);
    expect(calls().every((c) => c.cmd === "fetch_experiment_readiness")).toBe(true);
  });

  it("the other stages say they are not in the workspace yet and offer the way back", async () => {
    for (const [stage, name] of [[1, "Align"], [2, "Experiment"], [3, "Review"]] as const) {
      await render({ stage });
      expect(host.textContent).toContain(`Not yet in the workspace · ${name}`);
      await act(async () => button("Back to the classic view").click());
    }
    expect(onExit).toHaveBeenCalledTimes(3);
    expect(calls().length).toBe(0); // no stage but Prepare reads anything
  });

  it("Camera setup leaves for the classic Connect tab without a command", async () => {
    await render();
    const before = calls().length;
    await act(async () => button("Camera setup").click());
    expect(onOpenSetup).toHaveBeenCalledTimes(1);
    expect(calls().length).toBe(before);
  });
});
