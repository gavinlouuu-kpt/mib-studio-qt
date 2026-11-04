// @vitest-environment jsdom
// What entering a stage did to an eGrabber camera, and why the window could not be read or saved, is a line
// in the stage it concerns (#846), not only a log entry.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { StudioContext } from "../../studio/StudioContext";
import type { Studio } from "../../studio/useStudioController";
import { CameraStageNotice } from "./CameraStageNotice";
import type { StageName, StageNotice } from "./cameraStage";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let host: HTMLDivElement, root: Root;
beforeEach(() => { host = document.createElement("div"); document.body.append(host); root = createRoot(host); });
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

const show = async (stage: StageName, value: { stageNotice?: StageNotice | null; geometryError?: string; notice?: { level: "info" | "warn" | "error"; text: string } | null }) => {
  const studio = { stageNotice: value.stageNotice ?? null, geometryError: value.geometryError ?? "", cameraWin: { notice: value.notice ?? null } } as unknown as Studio;
  await act(async () => root.render(<StudioContext.Provider value={studio}><CameraStageNotice stage={stage} /></StudioContext.Provider>));
  return [...host.querySelectorAll('[data-testid="camera-stage-notice"]')].map((n) => ({ text: n.textContent, role: n.getAttribute("role"), cls: n.className }));
};

describe("camera stage notices", () => {
  it("shows the Overview's notice, the geometry error and the window save result there", async () => {
    const lines = await show("overview", {
      stageNotice: { stage: "overview", level: "error", text: "Failed to start camera. Please check camera connection and try again." },
      geometryError: "Camera geometry unavailable: transport closed",
      notice: { level: "info", text: "Camera window 992×128 at (368, 200) saved." },
    });
    expect(lines.map((l) => l.text)).toEqual([
      "Failed to start camera. Please check camera connection and try again.",
      "Camera geometry unavailable: transport closed",
      "Camera window 992×128 at (368, 200) saved.",
    ]);
    expect(lines[0].role).toBe("alert");
    expect(lines[2].role).toBe("status");
  });

  it("shows the Experiment's notice only in the Experiment stage", async () => {
    const notice: StageNotice = { stage: "experiment", level: "warn", text: "No Experiment camera script: the camera keeps its current window" };
    expect((await show("experiment", { stageNotice: notice })).map((l) => l.text)).toEqual([notice.text]);
    expect(await show("overview", { stageNotice: notice })).toEqual([]);
    // the geometry error and the window save are Overview lines
    expect(await show("experiment", { geometryError: "x", notice: { level: "info", text: "y" } })).toEqual([]);
  });
});
