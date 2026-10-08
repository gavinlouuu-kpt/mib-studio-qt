// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { RunWindowNotice } from "./RunWindowNotice";

let host: HTMLDivElement, root: Root;
beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

describe("camera mode notice (#501)", () => {
  it("tells the operator to place the run window, with a button that goes there", async () => {
    const place = vi.fn();
    await act(async () => root.render(<RunWindowNotice tab="experiment" windowPlaced={false} modeError={null} onPlaceWindow={place} onRetry={() => undefined} />));
    const alert = host.querySelector('[role="alert"]')!;
    expect(alert.textContent).toContain("Place the 512×96 run window in Camera & Alignment");
    await act(async () => host.querySelector("button")!.click());
    expect(place).toHaveBeenCalledTimes(1);
  });

  it("shows the backend's reason when the switch to Run was refused, and retries", async () => {
    const retry = vi.fn();
    await act(async () => root.render(<RunWindowNotice tab="experiment" windowPlaced modeError={{ mode: "run", message: "no Align preview arrived" }} onPlaceWindow={() => undefined} onRetry={retry} />));
    expect(host.querySelector('[role="alert"]')!.textContent).toContain("did not switch to Run: no Align preview arrived");
    await act(async () => host.querySelector("button")!.click());
    expect(retry).toHaveBeenCalledTimes(1);
  });

  it("shows an Align refusal on the Camera & Alignment tab only, and nothing when all is well", async () => {
    const err = { mode: "align" as const, message: "no Align preview arrived" };
    await act(async () => root.render(<RunWindowNotice tab="overview" windowPlaced={false} modeError={err} onPlaceWindow={() => undefined} onRetry={() => undefined} />));
    expect(host.querySelector('[role="alert"]')!.textContent).toContain("did not switch to Align");
    await act(async () => root.render(<RunWindowNotice tab="experiment" windowPlaced modeError={err} onPlaceWindow={() => undefined} onRetry={() => undefined} />));
    expect(host.querySelector('[role="alert"]')).toBeNull();
    await act(async () => root.render(<RunWindowNotice tab="experiment" windowPlaced modeError={null} onPlaceWindow={() => undefined} onRetry={() => undefined} />));
    expect(host.querySelector('[role="alert"]')).toBeNull();
  });
});
