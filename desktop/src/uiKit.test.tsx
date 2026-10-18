// @vitest-environment jsdom
// Smoke test for the @mib/ui wiring (vite.config.ts aliases + tsconfig paths):
// the app imports the kit by its package name, with its stylesheets, and
// renders kit components with the app's own React.
import "@mib/ui/fonts.css";
import "@mib/ui/style.css";
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { AppShell, StateChip, type StateTone } from "@mib/ui";

let host: HTMLDivElement, root: Root;
beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

describe("@mib/ui from desktop/src", () => {
  it("renders a kit shell and chip with the app's React", async () => {
    const onStageSelect = vi.fn();
    const tone: StateTone = "unknown";
    await act(async () => root.render(
      <AppShell brand="MIB Studio" stages={["Prepare", "Align", "Experiment", "Review"]} activeStage={0} onStageSelect={onStageSelect}>
        <StateChip tone={tone}>Unknown · not reported</StateChip>
      </AppShell>,
    ));
    expect(host.querySelector(".mib-shell")).not.toBeNull();
    expect(host.querySelector(".mib-chip")?.getAttribute("data-tone")).toBe("unknown");
    const tabs = host.querySelectorAll<HTMLButtonElement>(".mib-stages__tab");
    expect([...tabs].map((tab) => tab.textContent)).toEqual(["01Prepare", "02Align", "03Experiment", "04Review"]);
    await act(async () => tabs[1].click());
    expect(onStageSelect).toHaveBeenCalledWith(1);
  });
});
