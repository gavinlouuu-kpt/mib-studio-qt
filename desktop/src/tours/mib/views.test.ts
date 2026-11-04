// @vitest-environment jsdom
// MIB Studio tours: the view vocabulary and the disclosure helper. No bridge, no app.
import { describe, expect, it } from "vitest";
import { classicView, parseView, revealCollapsed, workspaceView } from "./views";

describe("tour views", () => {
  it("round-trips the workspace and classic shapes", () => {
    expect(parseView(workspaceView("align"))).toEqual({ shell: "workspace", stage: 1, sub: null });
    expect(parseView(workspaceView("experiment", "run-summary"))).toEqual({ shell: "workspace", stage: 2, sub: "run-summary" });
    expect(parseView(classicView("connect"))).toEqual({ shell: "classic", tab: "connect" });
  });
  it("refuses what it does not know", () => {
    expect(parseView({})).toBeNull();
    expect(parseView({ shell: "workspace", stage: "nowhere" })).toBeNull();
    expect(parseView({ shell: "workspace", stage: "experiment", sub: "nope" })).toBeNull();
    expect(parseView({ shell: "classic", tab: "settings" })).toBeNull();
    expect(parseView({ shell: "mobile" })).toBeNull();
  });
});

describe("revealCollapsed", () => {
  const page = (html: string) => { document.body.innerHTML = html; return document.body; };
  it("opens every closed <details> above an element, and nothing else", () => {
    page(`<details id="a"><summary>A</summary><details id="b"><summary>B</summary><button id="t">t</button></details></details><details id="c"><summary>C</summary></details>`);
    revealCollapsed(document.getElementById("t")!);
    expect((document.getElementById("a") as HTMLDetailsElement).open).toBe(true);
    expect((document.getElementById("b") as HTMLDetailsElement).open).toBe(true);
    expect((document.getElementById("c") as HTMLDetailsElement).open).toBe(false);
  });
  it("a <summary> never opens its own <details>", () => {
    page(`<details id="a"><summary id="s">A</summary><p>body</p></details>`);
    revealCollapsed(document.getElementById("s")!);
    expect((document.getElementById("a") as HTMLDetailsElement).open).toBe(false);
  });
});
