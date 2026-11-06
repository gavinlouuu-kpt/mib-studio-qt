// @vitest-environment jsdom
// #863 CONNECT-063: the Overview stage tab is disabled while an experiment is active, with Qt's sentence;
// every other tab stays reachable. (The Connect stage owns the sentence: areas/connect/cameraStage.ts.)
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { StudioContext } from "../../studio/StudioContext";
import type { Studio } from "../../studio/useStudioController";
import { OVERVIEW_DISABLED_TITLE } from "../connect/cameraStage";
import { StageTabs } from "./Workflow";

vi.mock("../connect/CameraActions", () => ({ CameraActions: () => null }));
Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

const stage = (title: string) => ({ title, statusLabel: "Ready", status: "ready", blocking: [] as string[], summary: `${title} summary` });
const studio = (over: Record<string, unknown>) => ({
  tab: "experiment", setTab: vi.fn(), expActive: false,
  stageByTab: { connect: stage("Connect"), overview: stage("Camera & Alignment"), experiment: stage("Experiment"), review: stage("Review") },
  ...over,
}) as unknown as Studio;

let host: HTMLDivElement, root: Root;
beforeEach(() => { host = document.createElement("div"); document.body.append(host); root = createRoot(host); });
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });
const tab = (name: string) => [...host.querySelectorAll<HTMLButtonElement>('[role="tab"]')].find((b) => b.textContent?.startsWith(name))!;

describe("the stage tabs during an experiment", () => {
  it("disable Overview with Qt's sentence and leave the others reachable", async () => {
    const value = studio({ expActive: true });
    await act(async () => root.render(<StudioContext.Provider value={value}><StageTabs /></StudioContext.Provider>));
    expect(tab("Camera").disabled).toBe(true);
    expect(tab("Camera").title).toBe("Overview tab is disabled while an experiment is active. Please stop the experiment first.");
    expect(OVERVIEW_DISABLED_TITLE).toBe(tab("Camera").title);
    for (const other of ["Connect", "Experiment", "Review"]) expect(tab(other).disabled).toBe(false);
    await act(async () => tab("Camera").click());
    expect(value.setTab).not.toHaveBeenCalled();
    await act(async () => tab("Connect").click());
    expect(value.setTab).toHaveBeenCalledWith("connect");
  });

  it("enable Overview when no experiment is active", async () => {
    const value = studio({});
    await act(async () => root.render(<StudioContext.Provider value={value}><StageTabs /></StudioContext.Provider>));
    expect(tab("Camera").disabled).toBe(false);
    expect(tab("Camera").title).toBe("Camera & Alignment summary");
    await act(async () => tab("Camera").click());
    expect(value.setTab).toHaveBeenCalledWith("overview");
  });
});
