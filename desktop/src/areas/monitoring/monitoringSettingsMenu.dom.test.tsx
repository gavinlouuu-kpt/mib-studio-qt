// @vitest-environment jsdom
// MON-082: Settings > Monitoring Settings... opens the dialog (it used to only switch to the Monitoring
// sub-tab, App.tsx:1440 before the area split).
import { act, useState } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { StudioContext } from "../../studio/StudioContext";
import { MenuBar } from "../shell/MenuBar";
import { ShellDialogs } from "../shell/Dialogs";
import { DEFAULT_MONITORING_SETTINGS, type MonitoringSettings } from "./monitoringSettings";

vi.mock("@tauri-apps/api/core", () => ({ invoke: vi.fn() }));
Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let host: HTMLDivElement, root: Root;
beforeEach(() => {
  host = document.createElement("div");
  document.body.append(host);
  root = createRoot(host);
});
afterEach(async () => {
  await act(async () => root.unmount());
  host.remove();
});

it("opens the Monitoring Settings dialog from the Settings menu without leaving the current view", async () => {
  const setTab = vi.fn(), setExpTab = vi.fn(), setConfigTab = vi.fn();
  function Shell() {
    const [showMonitoringSettings, setShowMonitoringSettings] = useState(false);
    const [monSettings, setMonSettings] = useState<MonitoringSettings>(DEFAULT_MONITORING_SETTINGS);
    const studio = {
      viewerOnly: false, operatingMode: "operator", setTab, setExpTab, setConfigTab,
      showMonitoringSettings, setShowMonitoringSettings, monSettings, setMonSettings,
    };
    return (
      <StudioContext.Provider value={studio as never}>
        <MenuBar />
        <ShellDialogs />
      </StudioContext.Provider>
    );
  }
  await act(async () => root.render(<Shell />));
  expect(host.querySelector('[data-tour="monitoring-settings-dialog"]')).toBeNull();
  const menu = (label: string) => Array.from(host.querySelectorAll("button")).find((b) => b.textContent === label)!;
  await act(async () => menu("Settings").click());
  await act(async () => menu("Monitoring Settings…").click());
  expect(host.querySelector('[data-tour="monitoring-settings-dialog"]')).not.toBeNull();
  expect(setTab).not.toHaveBeenCalled();
  expect(setExpTab).not.toHaveBeenCalled();
  expect(setConfigTab).not.toHaveBeenCalled();
});
