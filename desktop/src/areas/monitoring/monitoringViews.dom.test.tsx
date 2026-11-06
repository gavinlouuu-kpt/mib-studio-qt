// @vitest-environment jsdom
// The #866 views of the monitoring area: the run state widget, the standing Start readiness, the background
// thumbnail, and the places the run state locks other controls (stage tabs, Stop Camera, Raw Record, Select HDF File).
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { READINESS_GATE_STATUSES as S } from "../../bridgeContract";

const bridgeMock = vi.hoisted(() => ({ fetchBackground: vi.fn() }));
vi.mock("../../bridge", () => ({
  bridge: bridgeMock,
  isFrameReplyRace: (e: unknown) => e instanceof Error && (e.message === "FRAME_SOURCE_CHANGING" || e.message === "FRAME_REPLY_STALE"),
  mono8ToImageData: (_b: Uint8Array, w: number, h: number) => ({ width: w, height: h, data: new Uint8ClampedArray(w * h * 4) }),
}));
vi.mock("../connect/CameraActions", () => ({ CameraActions: () => <div data-testid="camera-actions" /> }));

import { StudioContext } from "../../studio/StudioContext";
import type { Studio } from "../../studio/useStudioController";
import { StageTabs } from "../shell/Workflow";
import { BackgroundThumbnail } from "./BackgroundThumbnail";
import { ReadinessAdvisories } from "./ReadinessAdvisories";
import { RunStatusBadge } from "./RunStatusBadge";
import { deriveRunPresentation, STAGE_LOCK_REASON } from "./runStatus";
import { summarizeReadiness, type StartGate } from "./startReadiness";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let host: HTMLDivElement, root: Root;
beforeEach(() => { host = document.createElement("div"); document.body.append(host); root = createRoot(host); });
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.clearAllMocks(); });

const gate = (id: string, status: number, reason: string, remediation = ""): StartGate => ({ id, status, reason, remediation });

describe("RunStatusBadge (MON-022)", () => {
  it("shows the glyph and the words, and says the state to assistive technology", async () => {
    await act(async () => root.render(<RunStatusBadge presentation={deriveRunPresentation({ cameraRunning: true, status: null })} />));
    const badge = host.querySelector<HTMLElement>("[data-tour='monitoring-run-status']")!;
    expect(badge.textContent).toBe("◉Camera running");
    expect(badge.getAttribute("role")).toBe("status");
    expect(badge.getAttribute("aria-label")).toBe("Run state: Camera running");
    expect(badge.title).toBe("Run state: Camera running (cameraRunning)");
    expect(badge.querySelector(".run-status-glyph")!.getAttribute("aria-hidden")).toBe("true");
  });
});

describe("ReadinessAdvisories (MON-028)", () => {
  const gates = [
    gate("camera.rate", S.Fail, "the camera allows at most 500 fps", "use a lower frame rate"),
    gate("storage.buffer", S.Warn, "the buffer holds 0.8 s", "free memory"),
    gate("camera.window", S.Pass, ""),
  ];
  it("lists what blocks Start and what only warns, with reasons and remediations", async () => {
    await act(async () => root.render(<ReadinessAdvisories summary={summarizeReadiness(gates)} notice="" />));
    expect(host.querySelector(".start-readiness-line")!.textContent).toBe("Start readiness: Blocked (1), 1 warning");
    const items = Array.from(host.querySelectorAll("li"));
    expect(items.map((li) => li.className)).toEqual(["blocked", "warning"]);
    expect(items[0].textContent).toBe("Blocked — camera.rate: the camera allows at most 500 fps → use a lower frame rate");
    expect(items[1].textContent).toContain("Warning — storage.buffer: the buffer holds 0.8 s");
    expect(host.querySelector("[data-tour='monitoring-readiness']")!.getAttribute("data-tone")).toBe("blocked");
  });
  it("says Ready to start when nothing is wrong, with no list", async () => {
    await act(async () => root.render(<ReadinessAdvisories summary={summarizeReadiness([gate("camera.window", S.Pass, "")])} notice="" />));
    expect(host.textContent).toBe("Start readiness: Ready to start");
    expect(host.querySelector("ul")).toBeNull();
  });
  it("shows the failure of the readiness poll as an alert and renders nothing before the first read", async () => {
    await act(async () => root.render(<ReadinessAdvisories summary={null} notice="The Start readiness check could not be read from the backend (3 attempts in a row): gone" />));
    expect(host.querySelector("[role='alert']")!.textContent).toContain("could not be read from the backend");
    await act(async () => root.render(<ReadinessAdvisories summary={null} notice="" />));
    expect(host.innerHTML).toBe("");
  });
});

describe("BackgroundThumbnail (MON-102)", () => {
  const packet = { valid: true, width: 8, height: 4, stride_bytes: 8, data: new Uint8Array(32) };
  let ctx: { putImageData: ReturnType<typeof vi.fn> };
  beforeEach(() => {
    ctx = { putImageData: vi.fn() };
    vi.spyOn(HTMLCanvasElement.prototype, "getContext").mockReturnValue(ctx as never);
  });
  afterEach(() => vi.restoreAllMocks());

  it("says No background set and pulls nothing while no background is set", async () => {
    await act(async () => root.render(<BackgroundThumbnail backgroundSet={false} revision={0} ready />));
    expect(host.querySelector(".bg-thumbnail-label")!.textContent).toBe("No background set");
    expect(bridgeMock.fetchBackground).not.toHaveBeenCalled();
    expect(host.querySelector("canvas")!.hidden).toBe(true);
  });
  it("pulls the image once when a background appears, draws it and names its size", async () => {
    bridgeMock.fetchBackground.mockResolvedValue(packet);
    await act(async () => root.render(<BackgroundThumbnail backgroundSet revision={1} ready />));
    expect(bridgeMock.fetchBackground).toHaveBeenCalledTimes(1);
    expect(ctx.putImageData).toHaveBeenCalledTimes(1);
    expect(host.querySelector(".bg-thumbnail-label")!.textContent).toBe("Background: 8x4");
    expect(host.querySelector("canvas")!.hidden).toBe(false);
    // The same state again: no new pull.
    await act(async () => root.render(<BackgroundThumbnail backgroundSet revision={1} ready />));
    expect(bridgeMock.fetchBackground).toHaveBeenCalledTimes(1);
  });
  it("pulls again when the configuration was re-read with a background (it was replaced)", async () => {
    bridgeMock.fetchBackground.mockResolvedValue(packet);
    await act(async () => root.render(<BackgroundThumbnail backgroundSet revision={1} ready />));
    await act(async () => root.render(<BackgroundThumbnail backgroundSet revision={2} ready />));
    expect(bridgeMock.fetchBackground).toHaveBeenCalledTimes(2);
  });
  it("goes back to No background set when the background is cleared", async () => {
    bridgeMock.fetchBackground.mockResolvedValue(packet);
    await act(async () => root.render(<BackgroundThumbnail backgroundSet revision={1} ready />));
    await act(async () => root.render(<BackgroundThumbnail backgroundSet={false} revision={2} ready />));
    expect(host.querySelector(".bg-thumbnail-label")!.textContent).toBe("No background set");
  });
  it("says why when the pull fails, but stays quiet about a pull this client discarded itself", async () => {
    bridgeMock.fetchBackground.mockRejectedValueOnce(new Error("backend gone"));
    await act(async () => root.render(<BackgroundThumbnail backgroundSet revision={1} ready />));
    expect(host.querySelector(".bg-thumbnail-label")!.textContent).toBe("Background set");
    expect(host.querySelector("[role='status']")!.textContent).toContain("backend gone");
    bridgeMock.fetchBackground.mockRejectedValueOnce(new Error("FRAME_SOURCE_CHANGING"));
    await act(async () => root.render(<BackgroundThumbnail backgroundSet revision={2} ready />));
    expect(host.querySelector("[role='status']")!.textContent).toContain("backend gone"); // unchanged: the race did not replace it
  });
});

describe("the stage tabs while a run is locked (MON-013, #585)", () => {
  const stage = (title: string) => ({ title, status: "ready", statusLabel: "Ready", blocking: [] as string[], summary: `${title} summary` });
  const studio = (over: Record<string, unknown>) => ({
    tab: "experiment", setTab: vi.fn(),
    stageByTab: { connect: stage("Connect"), overview: stage("Overview"), experiment: stage("Experiment"), review: stage("Review") },
    runLocked: false, ...over,
  }) as unknown as Studio;
  const tabs = () => Array.from(host.querySelectorAll<HTMLButtonElement>("button[role='tab']"));

  it("shuts Overview and Review, with the reason, and leaves Connect and Experiment open", async () => {
    await act(async () => root.render(<StudioContext.Provider value={studio({ runLocked: true })}><StageTabs /></StudioContext.Provider>));
    expect(tabs().map((b) => b.disabled)).toEqual([false, true, false, true]);
    expect(tabs()[1].title).toBe(STAGE_LOCK_REASON);
    expect(tabs()[3].title).toBe(STAGE_LOCK_REASON);
    expect(tabs()[2].title).toBe("Experiment summary");
  });
  it("opens them again when the run is over", async () => {
    await act(async () => root.render(<StudioContext.Provider value={studio({ runLocked: false })}><StageTabs /></StudioContext.Provider>));
    expect(tabs().every((b) => !b.disabled)).toBe(true);
  });
});
