// @vitest-environment jsdom
// #853: the Regenerate masks panel (a `.config-group`) sits inside a ViewerGate, a display:contents
// fieldset, since the viewer lock (#763 F19). `.subtab-body > .config-group { flex: 0 0 auto }` stopped
// matching it, the panel kept `flex: 1` and covered the Valid/Invalid/Charts views. This test takes the
// real rule out of widgets.css and the real ViewerGate markup, and checks the gated group is matched.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { readFileSync } from "node:fs";

// vitest stubs `?raw` CSS imports to "", so the stylesheet is read from disk (vitest runs from desktop/).
const widgetsCss = readFileSync("src/styles/widgets.css", "utf-8");

vi.mock("../transport/index", () => ({ transport: { kind: "tauri", invoke: vi.fn(), session: undefined } }));
import { ViewerGate } from "../components/ViewerGate";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let host: HTMLDivElement, root: Root;
beforeEach(() => { host = document.createElement("div"); document.body.append(host); root = createRoot(host); });
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

/** The selectors of every rule that sets `flex: 0 0 auto` and names a `.subtab-body > ... .config-group`. */
function stackedGroupSelectors(css: string): string[] {
  const stripped = css.replace(/\/\*[\s\S]*?\*\//g, "");
  const selectors: string[] = [];
  for (const [, head, body] of stripped.matchAll(/([^{}]+)\{([^{}]*)\}/g)) {
    if (!/flex:\s*0\s+0\s+auto/.test(body)) continue;
    for (const selector of head.split(",").map((s) => s.trim())) {
      if (selector.startsWith(".subtab-body >") && selector.endsWith(".config-group")) selectors.push(selector);
    }
  }
  return selectors;
}

describe("stacked config group in a sub-tab body (#853)", () => {
  it("the rule matches a bare group and a group inside a ViewerGate", async () => {
    const selectors = stackedGroupSelectors(widgetsCss);
    expect(selectors.length).toBeGreaterThanOrEqual(2);
    await act(async () => root.render(
      <div className="subtab-body">
        <section className="config-group" aria-label="bare" />
        <ViewerGate><section className="config-group" aria-label="gated" /></ViewerGate>
      </div>,
    ));
    for (const label of ["bare", "gated"]) {
      const group = host.querySelector(`[aria-label="${label}"]`)!;
      expect(selectors.some((selector) => group.matches(selector)), `${label} group matched by ${selectors.join(" | ")}`).toBe(true);
    }
  });

  it("the gate is the display:contents fieldset the selector relies on", () => {
    expect(widgetsCss).toMatch(/fieldset\.viewer-gate\s*\{[^}]*display:\s*contents/);
  });
});
