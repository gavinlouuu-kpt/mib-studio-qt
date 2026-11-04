// @vitest-environment jsdom
// HardwarePanel is the content of the right-hand hardware panel (HW-018): the pump/autofocus/pulse-generator
// controls and the Z stage, behind the viewer gate; the Z stage stays hidden on the PZ7035 (HW-072 rules).
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { Studio } from "../../studio/useStudioController";
import { StudioContext } from "../../studio/StudioContext";

vi.mock("../../components/HardwareControls", () => ({ HardwareControls: () => <section data-testid="hardware-controls">Pumps and autofocus</section> }));
vi.mock("../../components/StageControls", () => ({ StageControls: () => <section data-testid="stage-controls">Z stage</section> }));
import { HardwarePanel } from "./HardwarePanel";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let host: HTMLDivElement, root: Root;
beforeEach(() => { host = document.createElement("div"); document.body.append(host); root = createRoot(host); });
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

async function render(overrides: Record<string, unknown> = {}) {
  const studio = {
    ready: true, expActive: false, append: vi.fn(), caps: {}, operatingMode: "operator", triggerArmed: false,
    setTriggerArmed: vi.fn(), refreshCameraState: vi.fn(), pz7035: false, tab: "experiment", ...overrides,
  } as unknown as Studio;
  await act(async () => root.render(<StudioContext.Provider value={studio}><HardwarePanel /></StudioContext.Provider>));
}
const has = (id: string) => host.querySelector(`[data-testid="${id}"]`) !== null;

describe("HardwarePanel", () => {
  it("shows the pump panel and the Z stage whatever the stage tab", async () => {
    for (const tab of ["connect", "overview", "experiment", "review"]) {
      await render({ tab });
      expect(has("hardware-controls"), tab).toBe(true);
      expect(has("stage-controls"), tab).toBe(true);
      expect(host.querySelector("[hidden]"), tab).toBeNull();
    }
  });
  it("hides the Z stage on the PZ7035 and keeps the pump panel", async () => {
    await render({ pz7035: true });
    expect(has("hardware-controls")).toBe(true);
    expect(has("stage-controls")).toBe(false);
  });
  it("puts both behind the viewer gate", async () => {
    await render();
    const gate = host.querySelector("fieldset.viewer-gate")!;
    expect(gate.contains(host.querySelector('[data-testid="hardware-controls"]'))).toBe(true);
    expect(gate.contains(host.querySelector('[data-testid="stage-controls"]'))).toBe(true);
  });
});
