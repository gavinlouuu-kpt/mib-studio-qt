// @vitest-environment jsdom
import { act } from "react";
import { createRoot } from "react-dom/client";
import { expect, it, vi } from "vitest";
import { OfflineHelp, Markdown, offlineHelpMenu, releaseNotesForVersion } from "./OfflineHelp";
vi.mock("./transport/dialogs", () => ({ openUrl: vi.fn() }));
Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

it("Help menu opens offline manual and What's New", async () => {
  const host = document.createElement("div");
  const root = createRoot(host);
  const menu = offlineHelpMenu(kind => root.render(<OfflineHelp kind={kind} version="9.8.7" close={() => {}} />));
  expect(menu.map(item => item.label)).toEqual(["What's New", "User Manual"]);
  await act(async () => menu[1].onClick());
  expect(host.querySelector('[role="dialog"]')?.getAttribute("aria-label")).toBe("User Manual");
  expect(host.textContent).toContain("The workflow at a glance");
  await act(async () => (host.querySelector('a[href="connect.md"]') as HTMLAnchorElement).click());
  expect(host.textContent).toContain("Connect");
  expect(host.querySelector("img")?.getAttribute("src")).toBeTruthy();
  await act(async () => menu[0].onClick());
  expect(host.textContent).toContain("What's New");
  await act(async () => root.unmount());
});

it("renders a fixture with running notes before older notes", async () => {
  const text = releaseNotesForVersion({
    "/v9.8.6.md": "# Older", "/v9.8.7.md": "# Current\n\n**Offline Help** (#573)", "/v10.0.0.md": "# Future",
  }, "9.8.7");
  expect(text.indexOf("Current")).toBeLessThan(text.indexOf("Older"));
  expect(text).not.toContain("Future");
  const host = document.createElement("div");
  const root = createRoot(host);
  await act(async () => root.render(<Markdown text={text} navigate={() => {}} />));
  expect(host.querySelector("h1")?.textContent).toBe("Current");
  expect(host.querySelector("strong")?.textContent).toBe("Offline Help");
  await act(async () => root.unmount());
});
