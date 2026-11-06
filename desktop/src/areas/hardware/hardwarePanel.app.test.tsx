// @vitest-environment jsdom
// HW-018 / HW-042: the hardware panel is a right-hand panel beside every stage tab, collapsible, resizable,
// persisted, and it hides with a status-bar notice when the window is too narrow. These tests mount the real
// classic view (App) against a backend that answers only the shell preferences, so what is checked is the
// placement and the layout rules, not the device commands (those are the component tests).
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

let preferences: Record<string, unknown> = {};
const prefWrites: Record<string, unknown>[] = [];
vi.mock("@tauri-apps/api/core", () => ({
  invoke: vi.fn(async (cmd: string, args?: { preferences?: Record<string, unknown> }) => {
    if (cmd === "get_preferences") return structuredClone(preferences);
    if (cmd === "set_preferences") { preferences = structuredClone(args!.preferences!); prefWrites.push(preferences); return null; }
    throw new Error(`no backend in this test: ${cmd}`);
  }),
}));
vi.mock("@tauri-apps/api/window", () => ({
  getCurrentWindow: () => ({ onCloseRequested: async () => () => {}, close: async () => {} }),
}));
vi.mock("@tauri-apps/api/app", () => ({ getVersion: vi.fn(async () => "0.0.0") }));
vi.mock("@tauri-apps/plugin-dialog", () => ({ open: vi.fn(), save: vi.fn(), confirm: vi.fn() }));
vi.mock("@tauri-apps/plugin-opener", () => ({ openUrl: vi.fn() }));
import App from "../../App";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let host: HTMLDivElement, root: Root;

async function resizeWindow(width: number) {
  await act(async () => { Object.assign(window, { innerWidth: width }); window.dispatchEvent(new Event("resize")); });
}
async function mount(width = 1600) {
  Object.assign(window, { innerWidth: width });
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
  await act(async () => root.render(<App />));
  await act(async () => { await new Promise((r) => setTimeout(r, 0)); }); // the preferences read
}
const $ = <T extends Element = HTMLElement>(sel: string) => host.querySelector<T>(sel);
const body = () => $(".hw-dock-body")!;
const dock = () => $(".hw-dock")!;
const stageTabs = () => Array.from(host.querySelectorAll<HTMLButtonElement>(".stage-tab"));
const widthPx = () => Number.parseInt(body().style.width, 10);
async function click(el: Element) { await act(async () => (el as HTMLElement).click()); }
const mouse = (type: string, x: number) => new MouseEvent(type, { bubbles: true, clientX: x, button: 0 });
// The panel's own controls, to check they stay mounted (same nodes, drafts kept) while the panel is collapsed or
// hidden for space: an unmount would drop an in-flight Stop's command owner and the operator's typed values.
const portField = () => body().querySelector<HTMLInputElement>('input[type="text"]')!; // the Sample pump's serial port
const stopPump = () => Array.from(body().querySelectorAll("button")).find((b) => b.textContent === "Stop pump")!;
async function typeInto(input: HTMLInputElement, value: string) {
  const setValue = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!;
  await act(async () => { setValue.call(input, value); input.dispatchEvent(new Event("input", { bubbles: true })); });
}
async function drag(handle: Element, fromX: number, toX: number) {
  await act(async () => { handle.dispatchEvent(mouse("pointerdown", fromX)); });
  await act(async () => { handle.dispatchEvent(mouse("pointermove", toX)); });
  await act(async () => { handle.dispatchEvent(mouse("pointerup", toX)); });
}

// The test runtime's own localStorage is not usable (Node's experimental one shadows jsdom's): a Map stands in, as in
// the other component tests.
function stubStorage() {
  const saved = new Map<string, string>();
  vi.stubGlobal("localStorage", {
    getItem: (key: string) => saved.get(key) ?? null,
    setItem: (key: string, value: string) => { saved.set(key, String(value)); },
    removeItem: (key: string) => { saved.delete(key); },
    clear: () => saved.clear(),
  });
}
beforeEach(() => {
  preferences = {}; prefWrites.length = 0; stubStorage(); vi.useRealTimers();
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.unstubAllGlobals(); });

describe("hardware panel placement (HW-018)", () => {
  it("is a panel beside the main area, outside the tab body, with the device controls in it", async () => {
    await mount();
    expect(dock().parentElement).toBe($(".body"));
    expect(dock().previousElementSibling).toBe($("main.main"));
    expect($(".tab-body")!.contains(dock())).toBe(false);
    expect(body().hidden).toBe(false);
    expect(body().textContent).toContain("Pumps and autofocus");
    expect(body().textContent).toContain("Sample pump");
    expect(body().textContent).toContain("Sheath pump");
    expect(body().textContent).toContain("Z stage");
  });

  it("is visible and the same instance beside every stage tab", async () => {
    await mount();
    expect(stageTabs()).toHaveLength(4);
    const instance = body();
    for (const [i, tab] of stageTabs().entries()) {
      await click(tab);
      expect(tab.getAttribute("aria-selected"), `tab ${i} selected`).toBe("true");
      expect(body(), `tab ${i}`).toBe(instance);
      expect(body().hidden, `tab ${i}`).toBe(false);
      expect(body().textContent, `tab ${i}`).toContain("Pumps and autofocus");
    }
  });
});

describe("collapse and expand", () => {
  it("collapses and expands from the edge control, keeping the controls mounted", async () => {
    await mount();
    const toggle = $<HTMLButtonElement>(".hw-dock-toggle")!;
    expect(toggle.getAttribute("aria-label")).toBe("Hide hardware panel");
    expect(toggle.getAttribute("aria-expanded")).toBe("true");
    const instance = body();
    const field = portField(), stopButton = stopPump();
    await typeInto(field, "COM42");
    expect(field.value).toBe("COM42");
    await click(toggle);
    expect(body().hidden).toBe(true);
    expect(body()).toBe(instance);
    expect(portField()).toBe(field); // the controls stay mounted while collapsed
    expect(stopPump()).toBe(stopButton);
    expect(dock().dataset.state).toBe("collapsed");
    expect(toggle.getAttribute("aria-label")).toBe("Show hardware panel");
    expect(toggle.getAttribute("aria-expanded")).toBe("false");
    expect($(".hw-dock-handle")).toBeNull();
    await click(stageTabs()[2]);
    expect(body().hidden).toBe(true); // still collapsed on another tab
    await click(toggle);
    expect(body().hidden).toBe(false);
    expect(dock().dataset.state).toBe("open");
    expect(portField()).toBe(field);
    expect(stopPump()).toBe(stopButton);
    expect(field.value).toBe("COM42"); // the typed draft survived collapse and expand
  });

  it("collapsing is stored in localStorage and in the shell preferences", async () => {
    await mount();
    await click($(".hw-dock-toggle")!);
    expect(localStorage.getItem("mib.hardwarePanel.collapsed")).toBe("1");
    await act(async () => { await new Promise((r) => setTimeout(r, 0)); });
    expect(preferences.hardwarePanelCollapsed).toBe(true);
    await click($(".hw-dock-toggle")!);
    expect(localStorage.getItem("mib.hardwarePanel.collapsed")).toBe("0");
    await act(async () => { await new Promise((r) => setTimeout(r, 0)); });
    expect(preferences.hardwarePanelCollapsed).toBe(false);
  });

  it("keeps the other shell preferences when it writes its own", async () => {
    preferences = { sidebarCollapsed: true, somethingElse: { a: 1 } };
    await mount();
    await click($(".hw-dock-toggle")!);
    await act(async () => { await new Promise((r) => setTimeout(r, 0)); });
    expect(preferences).toMatchObject({ sidebarCollapsed: true, somethingElse: { a: 1 }, hardwarePanelCollapsed: true });
  });

  it("starts collapsed from localStorage, and the preferences file wins at boot like the left sidebar", async () => {
    localStorage.setItem("mib.hardwarePanel.collapsed", "1");
    await mount();
    expect(body().hidden).toBe(true);
    await act(async () => root.unmount()); host.remove();
    preferences = { hardwarePanelCollapsed: false }; // the durable copy says shown
    await mount();
    expect(body().hidden).toBe(false);
    expect(localStorage.getItem("mib.hardwarePanel.collapsed")).toBe("0");
  });
});

describe("width", () => {
  it("starts at the default width and restores a stored one", async () => {
    await mount();
    expect(widthPx()).toBe(360);
    await act(async () => root.unmount()); host.remove();
    localStorage.setItem("mib.hardwarePanel.width", "430");
    await mount();
    expect(widthPx()).toBe(430);
  });

  it("restores the width from the preferences file", async () => {
    preferences = { hardwarePanelWidth: 500 };
    await mount();
    expect(widthPx()).toBe(500);
    expect(localStorage.getItem("mib.hardwarePanel.width")).toBe("500");
  });

  it.each([["abc"], [""], ["-40"], ["0"], ["NaN"], ["999999"]])("falls back to the default for a stored %j", async (stored) => {
    localStorage.setItem("mib.hardwarePanel.width", stored);
    await mount();
    expect(widthPx()).toBe(360);
  });

  it.each([["120", 280], ["1500", 720]])("clamps a stored %s to the range (%i)", async (stored, expected) => {
    localStorage.setItem("mib.hardwarePanel.width", stored);
    await mount();
    expect(widthPx()).toBe(expected);
  });

  it("ignores bad values in the preferences file", async () => {
    localStorage.setItem("mib.hardwarePanel.width", "410");
    preferences = { hardwarePanelWidth: "wide", hardwarePanelCollapsed: "yes" };
    await mount();
    expect(widthPx()).toBe(410);
    expect(body().hidden).toBe(false);
  });

  it("survives unavailable storage", async () => {
    // Blocked for the panel's keys only: the left sidebar reads its own flag without a guard (not this panel's code).
    const blocked = (key: string) => { if (key.startsWith("mib.hardwarePanel")) throw new Error("blocked"); };
    vi.stubGlobal("localStorage", { getItem: (key: string) => { blocked(key); return null; }, setItem: (key: string) => { blocked(key); } });
    await mount();
    expect(widthPx()).toBe(360);
    await click($(".hw-dock-toggle")!); // still works, just not stored
    expect(body().hidden).toBe(true);
  });

  it("is resized by dragging the handle left (wider) and right (narrower), and stored", async () => {
    await mount();
    vi.useFakeTimers({ toFake: ["setTimeout", "clearTimeout"] }); // after the mount, which waits on a real timer
    const handle = $(".hw-dock-handle")!;
    await drag(handle, 1200, 1120);
    expect(widthPx()).toBe(440);
    expect(handle.getAttribute("aria-valuenow")).toBe("440");
    expect(localStorage.getItem("mib.hardwarePanel.width")).toBe("440");
    await drag(handle, 1200, 1290);
    expect(widthPx()).toBe(350);
    await act(async () => { await vi.advanceTimersByTimeAsync(400); });
    expect(preferences.hardwarePanelWidth).toBe(350); // the preferences file after a short pause
    expect(prefWrites.filter((w) => "hardwarePanelWidth" in w)).toHaveLength(1); // one write for the whole drag series
  });

  it("stops at the minimum and the maximum, and at what the main area leaves", async () => {
    await mount(1600);
    const handle = $(".hw-dock-handle")!;
    await drag(handle, 1200, 100);
    expect(widthPx()).toBe(720);
    await drag(handle, 1200, 2200);
    expect(widthPx()).toBe(280);
    await resizeWindow(1100); // 1100 - 264 (sidebar) - 28 (edge) - 560 (main) = 248 left
    expect(widthPx()).toBe(248);
    expect(handle.getAttribute("aria-valuemax")).toBe("248");
  });

  it("is resized from the keyboard: arrows 16 px (Shift 64 px), Home and End for the bounds", async () => {
    await mount();
    const handle = $(".hw-dock-handle")!;
    const press = (key: string, shiftKey = false) => act(async () => { handle.dispatchEvent(new KeyboardEvent("keydown", { key, shiftKey, bubbles: true })); });
    await press("ArrowLeft"); expect(widthPx()).toBe(376);
    await press("ArrowRight"); await press("ArrowRight"); expect(widthPx()).toBe(344);
    await press("ArrowLeft", true); expect(widthPx()).toBe(408);
    await press("Home"); expect(widthPx()).toBe(280);
    await press("End"); expect(widthPx()).toBe(720);
    expect(localStorage.getItem("mib.hardwarePanel.width")).toBe("720");
  });

  it("double-click on the handle restores the default width", async () => {
    localStorage.setItem("mib.hardwarePanel.width", "600");
    await mount();
    expect(widthPx()).toBe(600);
    await act(async () => { $(".hw-dock-handle")!.dispatchEvent(new MouseEvent("dblclick", { bubbles: true })); });
    expect(widthPx()).toBe(360);
  });
});

describe("narrow window (HW-042)", () => {
  const notice = () => $(".statusbar .hw-panel-notice")!.textContent;
  const NOTICE = "Window too narrow for the hardware panel; enlarge the window to show it.";

  it("fits at the usual widths: full width on a wide window, narrower on a medium one", async () => {
    await mount(1600);
    expect(widthPx()).toBe(360);
    expect(notice()).toBe("");
    await resizeWindow(1180); // 1180 - 264 - 28 - 560 = 328
    expect(widthPx()).toBe(328);
    expect(body().hidden).toBe(false);
    expect(notice()).toBe("");
  });

  it("hides with the status-bar notice when even the compact panel does not fit, and comes back by itself", async () => {
    await mount(1600);
    const field = portField(), stopButton = stopPump();
    await typeInto(field, "COM42");
    await resizeWindow(1000);
    expect(body().hidden).toBe(true);
    expect(portField()).toBe(field); // the controls stay mounted while hidden for space
    expect(stopPump()).toBe(stopButton);
    expect(dock().dataset.state).toBe("hidden-for-space");
    expect(notice()).toBe(NOTICE);
    expect($(".statusbar .hw-panel-notice")!.getAttribute("role")).toBe("status");
    await resizeWindow(1600);
    expect(body().hidden).toBe(false);
    expect(widthPx()).toBe(360); // the preference was kept
    expect(notice()).toBe("");
    expect(portField()).toBe(field);
    expect(stopPump()).toBe(stopButton);
    expect(field.value).toBe("COM42"); // the typed draft survived the hide for space
  });

  it("does not store the automatic hide as the operator's choice", async () => {
    await mount(1000);
    expect(body().hidden).toBe(true);
    expect(localStorage.getItem("mib.hardwarePanel.collapsed")).toBeNull();
    await act(async () => { await new Promise((r) => setTimeout(r, 0)); });
    expect(prefWrites.some((w) => "hardwarePanelCollapsed" in w)).toBe(false);
    await resizeWindow(1600);
    expect(body().hidden).toBe(false);
  });

  it("can be expanded by hand in a narrow window (compact width), until the window has room again", async () => {
    await mount(1000);
    await click($(".hw-dock-toggle")!);
    expect(body().hidden).toBe(false);
    expect(widthPx()).toBe(240);
    expect(notice()).toBe("");
    expect(localStorage.getItem("mib.hardwarePanel.collapsed")).toBe("0");
    await resizeWindow(1600);
    expect(widthPx()).toBe(360);
    await resizeWindow(1000); // narrow again: hidden for space again, the hand-show does not stick
    expect(body().hidden).toBe(true);
    expect(notice()).toBe(NOTICE);
  });

  it("collapsing by hand in a narrow window clears the notice and is stored", async () => {
    await mount(1000);
    expect(notice()).toBe(NOTICE);
    await click($(".hw-dock-toggle")!); // shown by hand
    await click($(".hw-dock-toggle")!); // hidden by hand
    expect(body().hidden).toBe(true);
    expect(notice()).toBe("");
    expect(localStorage.getItem("mib.hardwarePanel.collapsed")).toBe("1");
    await resizeWindow(1600);
    expect(body().hidden).toBe(true); // the operator's choice stands
  });

  it("gets its room back when the telemetry sidebar is hidden", async () => {
    await mount(1000);
    expect(body().hidden).toBe(true);
    await click($(".sidebar-toggle")!);
    expect(body().hidden).toBe(false); // 1000 - 14 - 28 - 560 = 398 left
    expect(widthPx()).toBe(360);
    expect(notice()).toBe("");
  });
});
