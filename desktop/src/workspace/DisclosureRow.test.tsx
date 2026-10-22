// @vitest-environment jsdom
// The disclosure row (O1b; kit gap #715): a row with an issue is open and cannot be collapsed; an
// automatic open is closable; the operator's choice is told apart from an automatic open; the
// body is rendered but hidden by a closed <details>, which isShown accounts for.
import { act, useState } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { DisclosureRow, isShown, rowOpen, type DisclosureRowProps } from "./DisclosureRow";

let host: HTMLDivElement, root: Root;
beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

/** A row with its choice held, as the summary holds it. */
function Row({ state }: { state: DisclosureRowProps["state"] }) {
  const [choice, setChoice] = useState<boolean | undefined>(undefined);
  return (
    <DisclosureRow rowKey="camera" title="Camera and acquisition" summary="Mock camera (chosen)" chip={{ tone: "blocked", text: "Fail · Camera source" }}
      state={state} choice={choice} onChoice={setChoice}>
      <p data-testid="body">Fix: select the mock camera explicitly</p>
    </DisclosureRow>
  );
}
const render = (state: DisclosureRowProps["state"]) => act(async () => root.render(<Row state={state} />));
const details = () => host.querySelector<HTMLDetailsElement>("details[data-row=camera]")!;
const summary = () => details().querySelector<HTMLElement>(":scope > summary")!;
const body = () => host.querySelector("[data-testid=body]")!;
const click = () => act(async () => summary().click());

describe("DisclosureRow", () => {
  it("closed by default; the operator opens and closes it, and the body is hidden while closed", async () => {
    await render({ issue: false });
    expect(details().open).toBe(false);
    expect(details().getAttribute("data-open")).toBe("closed");
    expect(host.textContent).toContain("Fix: select"); // rendered, so textContent alone proves nothing
    expect(isShown(body())).toBe(false);
    expect(isShown(summary().querySelector(".mib-chip")!)).toBe(true); // a closed row shows its summary
    await click();
    expect(details().open).toBe(true);
    expect(details().getAttribute("data-open")).toBe("user");
    expect(isShown(body())).toBe(true);
    await click();
    expect(details().getAttribute("data-open")).toBe("closed");
  });

  it("a row with an issue is open, says so, and a click does not close it", async () => {
    await render({ issue: true });
    expect(details().open).toBe(true);
    expect(details().getAttribute("data-open")).toBe("issue");
    expect(summary().getAttribute("aria-disabled")).toBe("true");
    await click();
    expect(details().open).toBe(true);
    expect(isShown(body())).toBe(true);
  });

  it("an automatic open is closable; once an issue clears, the row returns to the operator's choice", async () => {
    await render({ issue: false, auto: true });
    expect(details().getAttribute("data-open")).toBe("auto");
    await click();
    expect(details().open).toBe(false);
    await render({ issue: true, auto: true });
    expect(details().getAttribute("data-open")).toBe("issue");
    await render({ issue: false, auto: true });
    expect(details().open).toBe(false); // the operator closed it before the issue
  });

  it("a browser-initiated close of a row with an issue is undone; an open by the browser is the operator's", async () => {
    await render({ issue: true });
    await act(async () => { details().open = false; details().dispatchEvent(new Event("toggle")); });
    expect(details().open).toBe(true);
    await render({ issue: false });
    await act(async () => { details().open = false; details().dispatchEvent(new Event("toggle")); });
    expect(details().getAttribute("data-open")).toBe("closed");
    await act(async () => { details().open = true; details().dispatchEvent(new Event("toggle")); });
    expect(details().getAttribute("data-open")).toBe("user");
  });

  it("rowOpen: issue, then the choice, then auto", () => {
    expect(rowOpen({ issue: true }, false)).toBe(true);
    expect(rowOpen({ issue: false, auto: true }, false)).toBe(false);
    expect(rowOpen({ issue: false, auto: true }, undefined)).toBe(true);
    expect(rowOpen({ issue: false }, undefined)).toBe(false);
  });
});
