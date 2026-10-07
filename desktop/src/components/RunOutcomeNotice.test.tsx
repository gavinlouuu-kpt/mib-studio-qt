// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { RunOutcomeNotice } from "./RunOutcomeNotice";
import type { RunOutcome } from "../runOutcome";

let host: HTMLDivElement, root: Root;
const outcome = (o: Partial<RunOutcome>): RunOutcome =>
  ({ severity: "ok", headline: "Run complete: all 10 admitted frames reconciled.", losses: [], admitted: 10, lossFraction: null, reason: "", attention: false, ...o });

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

describe("run outcome notice (#549)", () => {
  it("is an alert for an undeclared loss, with the backend's reason on hover", async () => {
    await act(async () => root.render(<RunOutcomeNotice outcome={outcome({ severity: "loss", headline: "Run finished with undeclared loss of 1 frame.", reason: "undeclared loss: storeMalformed=1" })} />));
    const p = host.querySelector("p")!;
    expect(p.getAttribute("role")).toBe("alert");
    expect(p.className).toContain("loss");
    expect(p.textContent).toContain("undeclared loss");
    expect(p.getAttribute("title")).toContain("storeMalformed=1");
  });

  it("keeps a declared partial result quiet, but alerts when malformed frames are above the warning fraction", async () => {
    await act(async () => root.render(<RunOutcomeNotice outcome={outcome({ severity: "partial", headline: "Run finished with a declared partial result: 1 frame was malformed." })} />));
    expect(host.querySelector("p")!.getAttribute("role")).toBe("status");
    await act(async () => root.render(<RunOutcomeNotice outcome={outcome({ severity: "partial", attention: true, headline: "Run finished with a declared partial result: 40 frames were malformed." })} />));
    expect(host.querySelector("p")!.getAttribute("role")).toBe("alert");
    expect(host.querySelector("p")!.className).toContain("attention");
  });

  it("is a quiet status for a clean run and nothing without an outcome", async () => {
    await act(async () => root.render(<RunOutcomeNotice outcome={outcome({})} />));
    expect(host.querySelector("p")!.getAttribute("role")).toBe("status");
    await act(async () => root.render(<RunOutcomeNotice outcome={null} />));
    expect(host.querySelector("p")).toBeNull();
  });
});
