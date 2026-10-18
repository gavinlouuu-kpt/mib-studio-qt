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
/** The value cell of a gate row, found by the gate id (the label is the plain name). */
const gateRow = (id: string, scope = GATES) => host.querySelector(`${scope} [data-gate-id="${id}"]`)?.closest("dt")?.nextElementSibling as HTMLElement | undefined;
const verdictChip = () => host.querySelector("[data-testid=workspace-verdict]")!;

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
  it("shows the gates read-only under plain labels, the destination as Unavailable, and one verdict", async () => {
    await render();
    const label = host.querySelector(`${GATES} [data-gate-id="storage.output"]`)!;
    expect(label.textContent).toBe("Destination filestorage.output"); // plain label, then the id for support
    expect(label.getAttribute("title")).toBe("storage.output");
    const value = gateRow("storage.output")!;
    const chip = value.querySelector(".mib-chip")!;
    expect(chip.textContent).toBe("Unavailable · no output path chosen yet");
    expect(chip.getAttribute("data-tone")).toBe("unknown");
    expect(value.textContent).toContain("Fix: choose an HDF5 destination");
    expect(gateRow("storage.output", "[data-testid=workspace-prepare]")!.textContent).toContain("Unavailable · no output path chosen yet"); // the destination card
    // One verdict, the same words in the header and in the card.
    expect(verdictChip().textContent).toBe("Ready · destination checked at Start");
    expect(verdictChip().getAttribute("data-tone")).toBe("confirmed");
    const cardChip = [...host.querySelectorAll(".mib-card")].find((c) => c.textContent?.startsWith("Ready to operate?"))!.querySelector(".mib-card__status .mib-chip")!;
    expect(cardChip.textContent).toBe(verdictChip().textContent);
    expect(host.querySelector("[data-testid=workspace-footnote]")!.textContent).toBe("Unknown is never Pass. Unsupported never blocks. Start re-evaluates every gate with the chosen file.");
  });

  it("an unknown gate keeps its raw id as the label, never hidden", async () => {
    gates = [{ id: "future.gate", status: G.Warn, reason: "new", remediation: "" }, NO_PATH];
    await render();
    expect(host.querySelector(`${GATES} [data-gate-id="future.gate"]`)!.textContent).toBe("future.gate");
    expect(verdictChip().textContent).toBe("Ready with warnings · 1 warning");
  });

  it("is never a pass before the first read", async () => {
    vi.mocked(invoke).mockImplementation(() => new Promise(() => {})); // never answers
    await render();
    expect(verdictChip().textContent).toBe("Ready to confirm · gates not read yet");
    expect(verdictChip().getAttribute("data-tone")).toBe("unknown");
    expect(host.querySelector("[data-testid=workspace-start-refused]")).toBeNull();
  });

  it("counts blocking gates and checks in one verdict when the checklist blocks", async () => {
    gates = [{ id: "camera.source", status: G.Unavailable, reason: "no camera source configured", remediation: "" }, { id: "processing.core", status: G.Fail, reason: "pin", remediation: "" }, NO_PATH];
    await render({ preflightInput: { ...INPUT, coreKnown: false, coreValid: false } });
    expect(verdictChip().textContent).toBe("Not ready · 1 failed, 2 unknown");
    expect(verdictChip().getAttribute("data-tone")).toBe("blocked");
    expect(host.querySelector("[data-testid=workspace-verdict-detail]")!.textContent).toBe(
      "Failed: Processing core. Unknown: Processing core / trust, Camera source. The destination is checked at Start, with the chosen file.");
    expect(host.querySelector("[data-testid=workspace-start-refused]")).toBeNull(); // Confirm is unavailable; its Notice says why
  });

  it("checklist passes but a gate fails: Ready to confirm · Start blocked, Confirm stays available, and a Notice says what Start will refuse", async () => {
    gates = [{ id: "camera.source", status: G.Pass, reason: "", remediation: "" },
      { id: "processing.core", status: G.Fail, reason: "processing core pin not satisfied", remediation: "activate the pinned core" }, NO_PATH];
    await render();
    expect(verdictChip().textContent).toBe("Ready to confirm · Start blocked: Processing core");
    expect(verdictChip().getAttribute("data-tone")).toBe("warn");
    const cardChip = [...host.querySelectorAll(".mib-card")].find((c) => c.textContent?.startsWith("Ready to operate?"))!.querySelector(".mib-card__status .mib-chip")!;
    expect(cardChip.textContent).toBe(verdictChip().textContent);
    expect(cardChip.getAttribute("data-tone")).toBe("warn");
    expect(button("Confirm readiness").disabled).toBe(false); // the checklist drives Confirm, as in the classic Preflight
    expect(host.querySelector("[data-testid=workspace-confirm-why]")).toBeNull();
    const refused = host.querySelector("[data-testid=workspace-start-refused]")!;
    expect(refused.getAttribute("data-tone")).toBe("warn");
    expect(refused.textContent).toBe("Start blocked · 1 gateStart will be refused until: Processing core — activate the pinned core.");
    expect(host.textContent).not.toContain("Not ready");
  });

  it("a failed read clears the gates instead of showing stale ones", async () => {
    await render();
    expect(gateRow("camera.source")).toBeDefined();
    vi.mocked(invoke).mockRejectedValue(new Error("bridge gone"));
    await advance(PREPARE_READINESS_POLL_MS);
    expect(gateRow("camera.source")).toBeUndefined();
    expect(verdictChip().getAttribute("data-tone")).toBe("unknown");
  });

  it("renders a check not read yet as Unknown, not failed, and keeps Confirm disabled", async () => {
    await render({ preflightInput: { ...INPUT, coreKnown: false, coreValid: false } });
    const core = row("Processing core / trust · required")!;
    expect(core.textContent).toBe("Unknown · expected 2.1.0 · Processing-core status not read yet.");
    expect(core.className).toContain("mib-facts__value--unknown");
    expect(core.className).not.toContain("blocked");
    expect(host.textContent).toContain("Required checks: 1 of 2 ready · 1 unknown");
    expect(button("Confirm readiness").disabled).toBe(true);
    // The disabled button says why, in plain words, next to it.
    const why = host.querySelector("[data-testid=workspace-confirm-why]")!;
    expect(why.getAttribute("data-tone")).toBe("unknown");
    expect(why.textContent).toBe("Confirm readiness is unavailable1 required check not read yet or not verified: Processing core / trust.");
    expect(verdictChip().textContent).toBe("Not ready · 1 unknown");
    // Storage that nothing reported is unknown, not "not required".
    expect(row("Storage destination")!.textContent).toMatch(/^Unknown · /);
  });

  it("confirms only on the explicit press, and the confirmation sends nothing to the backend", async () => {
    await render();
    const before = calls().length;
    expect(button("Confirm readiness").disabled).toBe(false);
    expect(host.querySelector("[data-testid=workspace-confirm-why]")).toBeNull();
    await act(async () => button("Confirm readiness").click());
    expect(onConfirm).toHaveBeenCalledTimes(1);
    expect(calls().length).toBe(before);
    await render({ confirmedFor: "sig-now" });
    expect(row("Preflight confirmation")!.textContent).toBe("Confirmed for this setup");
    expect(button("Confirm readiness").disabled).toBe(true);
    await render({ confirmedFor: "sig-before" });
    expect(row("Preflight confirmation")!.textContent).toContain("Setup changed since the confirmation");
  });

  it("a required warning blocks Confirm even when no backend gate fails: gates never unblock a check (#548)", async () => {
    gates = [{ id: "camera.source", status: G.Pass, reason: "", remediation: "" }, { id: "camera.session", status: G.Warn, reason: "w", remediation: "" }, NO_PATH];
    await render({ preflightInput: { ...INPUT, cameraExpected: "Other camera" } });
    expect(gates.some((g) => g.status === G.Fail)).toBe(false);
    expect(button("Confirm readiness").disabled).toBe(true);
    expect(host.querySelector("[data-testid=workspace-confirm-why]")!.textContent).toContain("1 required check with a warning: Camera");
    expect(verdictChip().textContent).toBe("Not ready · 2 warnings");
  });

  it("names the lock when a run is active", async () => {
    await render({ experimentActive: true, confirmLockedReason: "Setup is locked while an experiment is active" });
    expect(button("Confirm readiness").disabled).toBe(true);
    expect(host.querySelector("[data-testid=workspace-confirm-why]")!.textContent).toContain("Setup is locked while an experiment is active");
    expect(verdictChip().textContent).toBe("Readiness paused · a run is active"); // never "Ready to confirm" while Confirm is locked
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
