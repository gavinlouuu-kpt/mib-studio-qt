// @vitest-environment jsdom
import { act, useState } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { StudioContext } from "../../studio/StudioContext";
import { MonitoringSettingsDialog, MonitoringSettingsDialogView } from "./MonitoringSettingsDialog";
import {
  applyMonitoringSettings, DEFAULT_MONITORING_SETTINGS, MONITORING_SETTINGS_GROUPS, type MonitoringSettings, type SettingsGroup,
} from "./monitoringSettings";

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

const input = (tour: string) => host.querySelector<HTMLInputElement>(`[data-tour="${tour}"]`)!;
const button = (label: string) => Array.from(host.querySelectorAll("button")).find((b) => b.textContent === label)!;
const type = (el: HTMLInputElement, value: string) =>
  act(async () => {
    // React tracks the input's value: set it through the native setter so onChange sees the change.
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(el, value);
    el.dispatchEvent(new Event("input", { bubbles: true }));
  });

function view(over: Partial<React.ComponentProps<typeof MonitoringSettingsDialogView>> = {}) {
  const onApply = vi.fn((v) => applyMonitoringSettings(DEFAULT_MONITORING_SETTINGS, v).ignored);
  const onClose = vi.fn();
  return { onApply, onClose, element: <MonitoringSettingsDialogView settings={DEFAULT_MONITORING_SETTINGS} onApply={onApply} onClose={onClose} {...over} /> };
}

describe("Monitoring Settings dialog fields (MON-082, MON-087..091)", () => {
  it("is a modal dialog with the tour anchor and the Qt fields, ranges, decimals and defaults", async () => {
    await act(async () => root.render(view().element));
    const dialog = host.querySelector('[role="dialog"]')!;
    expect(dialog.getAttribute("aria-modal")).toBe("true");
    expect(dialog.getAttribute("aria-label")).toBe("Monitoring Settings");
    expect(dialog.getAttribute("data-tour")).toBe("monitoring-settings-dialog");
    expect(host.querySelector(".modal-backdrop")).not.toBeNull();
    const labels = Array.from(host.querySelectorAll(".mon-settings-label")).map((l) => l.textContent);
    expect(labels).toEqual([
      // Qt's form: the density fields first (MON-083..085), then the axis groups.
      "KDE bandwidth factor", "KDE update interval", "Core contour",
      "X (Area) min", "X (Area) max", "Y (Deformability) min", "Y (Deformability) max",
      "X (Ring ratio) min", "X (Ring ratio) max", "Y (Frequency) max", "Bin width",
    ]);
    const legends = Array.from(host.querySelectorAll("legend")).map((l) => l.textContent);
    expect(legends).toEqual(["Density (KDE)", "Scatter plot axis (Area vs Deformability)", "Histogram axis (Ring-ratio distribution)"]);
    const spec = Array.from(host.querySelectorAll<HTMLInputElement>("input")).map((i) => [i.value, i.min, i.max, i.step]);
    expect(spec).toEqual([
      ["1.00", "0.2", "5", "0.1"], ["2000", "500", "60000", "500"], ["90", "5", "100", "5"],
      ["0.00", "0", "100000", "1"], ["1000.00", "0", "100000", "1"],
      ["0.000", "0", "10", "1"], ["1.000", "0", "10", "1"],
      ["15.00", "0", "100", "1"], ["25.00", "0", "100", "1"],
      // The bin width's 0.01 minimum is not a multiple of its 0.1 step: no min attribute, so the arrows
      // step 0.5, 0.6 like Qt; the range is enforced on Apply.
      ["100", "1", "1000000", "1"], ["0.50", "", "10", "0.1"],
    ]);
    expect(document.activeElement).toBe(input("monitoring-settings-kde-bandwidth"));
  });

  it("starts from the current settings", async () => {
    const current: MonitoringSettings = { ...DEFAULT_MONITORING_SETTINGS, scatterXMax: 2500, histogramXMin: 12.5, histogramBinWidth: 0.25 };
    await act(async () => root.render(view({ settings: current }).element));
    expect(input("monitoring-settings-scatter-x-max").value).toBe("2500.00");
    expect(input("monitoring-settings-histogram-x-min").value).toBe("12.50");
    expect(input("monitoring-settings-histogram-bin-width").value).toBe("0.25");
  });
});

describe("OK / Cancel / Apply", () => {
  it("Apply applies and keeps the dialog open; OK applies and closes; Cancel only closes", async () => {
    const v = view();
    await act(async () => root.render(v.element));
    await type(input("monitoring-settings-scatter-x-max"), "2000");
    await act(async () => button("Apply").click());
    expect(v.onApply).toHaveBeenCalledOnce();
    expect(v.onApply.mock.calls[0][0]).toMatchObject({ scatterXMin: 0, scatterXMax: 2000, histogramBinWidth: 0.5 });
    expect(v.onClose).not.toHaveBeenCalled();
    expect(input("monitoring-settings-scatter-x-max").value).toBe("2000.00");

    await type(input("monitoring-settings-histogram-y-max"), "500");
    await act(async () => button("OK").click());
    expect(v.onApply).toHaveBeenCalledTimes(2);
    expect(v.onApply.mock.calls[1][0]).toMatchObject({ histogramYMax: 500 });
    expect(v.onClose).toHaveBeenCalledOnce();
  });

  it("Cancel and Escape close without applying; a click on the backdrop does not close", async () => {
    const v = view();
    await act(async () => root.render(v.element));
    await type(input("monitoring-settings-scatter-x-max"), "2000");
    await act(async () => host.querySelector<HTMLElement>(".modal-backdrop")!.click());
    expect(v.onClose).not.toHaveBeenCalled();
    await act(async () => button("Cancel").click());
    expect(v.onClose).toHaveBeenCalledOnce();
    await act(async () => {
      host.querySelector('[role="dialog"]')!.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", bubbles: true }));
    });
    expect(v.onClose).toHaveBeenCalledTimes(2);
    expect(v.onApply).not.toHaveBeenCalled();
  });

  it("Enter in a field is OK", async () => {
    const v = view();
    await act(async () => root.render(v.element));
    await act(async () => {
      host.querySelector("form")!.dispatchEvent(new Event("submit", { bubbles: true, cancelable: true }));
    });
    expect(v.onApply).toHaveBeenCalledOnce();
    expect(v.onClose).toHaveBeenCalledOnce();
  });

  it("clamps to the range, rounds to the decimals and keeps the last value for text that is not a number", async () => {
    const v = view();
    await act(async () => root.render(v.element));
    await type(input("monitoring-settings-scatter-x-max"), "999999");
    await type(input("monitoring-settings-scatter-y-max"), "0.12345");
    await type(input("monitoring-settings-histogram-bin-width"), "");
    await type(input("monitoring-settings-histogram-y-max"), "abc");
    await act(async () => button("Apply").click());
    expect(v.onApply.mock.calls[0][0]).toMatchObject({ scatterXMax: 100000, scatterYMax: 0.123, histogramBinWidth: 0.5, histogramYMax: 100 });
    expect(input("monitoring-settings-scatter-x-max").value).toBe("100000.00");
    expect(input("monitoring-settings-scatter-y-max").value).toBe("0.123");
    expect(input("monitoring-settings-histogram-bin-width").value).toBe("0.50");
    expect(input("monitoring-settings-histogram-y-max").value).toBe("100");
  });

  it("applies the valid ranges and says which one was not applied (minimum not below maximum); OK then stays open", async () => {
    const v = view();
    await act(async () => root.render(v.element));
    await type(input("monitoring-settings-scatter-x-min"), "900");
    await type(input("monitoring-settings-scatter-x-max"), "800");
    await type(input("monitoring-settings-scatter-y-max"), "0.5");
    await act(async () => button("OK").click());
    expect(v.onApply.mock.calls[0][0]).toMatchObject({ scatterXMin: 900, scatterXMax: 800, scatterYMax: 0.5 });
    expect(v.onClose).not.toHaveBeenCalled();
    expect(host.querySelector('[role="status"]')!.textContent).toContain("Not applied: Scatter X (Area)");
    // Fixing the range clears the note, and OK closes.
    await type(input("monitoring-settings-scatter-x-max"), "1200");
    await act(async () => button("OK").click());
    expect(host.querySelector('[role="status"]')).toBeNull();
    expect(v.onClose).toHaveBeenCalledOnce();
  });
});

describe("groups (room for the Density (KDE) section)", () => {
  it("renders whatever groups it is given, with a field's unit", async () => {
    const extra: SettingsGroup = {
      id: "density",
      title: "Density (KDE)",
      fields: [{ ...MONITORING_SETTINGS_GROUPS[2].fields[3], label: "Bandwidth", tour: "monitoring-settings-kde-bandwidth", suffix: "×" }],
      pairs: [],
    };
    await act(async () => root.render(view({ groups: [extra, ...MONITORING_SETTINGS_GROUPS] }).element));
    const legends = Array.from(host.querySelectorAll("legend")).map((l) => l.textContent);
    expect(legends[0]).toBe("Density (KDE)");
    expect(host.querySelectorAll('[data-tour="monitoring-settings-density"]')).toHaveLength(2); // the extra group and the real one
    expect(host.querySelector(".mon-settings-suffix")!.textContent).toBe("×");
    expect(document.activeElement).toBe(input("monitoring-settings-kde-bandwidth"));
  });
});

describe("the dialog in the studio state", () => {
  function Studio({ initialOpen }: { initialOpen: boolean }) {
    const [showMonitoringSettings, setShowMonitoringSettings] = useState(initialOpen);
    const [monSettings, setMonSettings] = useState<MonitoringSettings>(DEFAULT_MONITORING_SETTINGS);
    return (
      <StudioContext.Provider value={{ showMonitoringSettings, setShowMonitoringSettings, monSettings, setMonSettings } as never}>
        <button onClick={() => setShowMonitoringSettings(true)}>open</button>
        <output data-testid="x-max">{monSettings.scatterXMax}:{monSettings.scatterEpoch}</output>
        <MonitoringSettingsDialog />
      </StudioContext.Provider>
    );
  }
  const readout = () => host.querySelector('[data-testid="x-max"]')!.textContent;

  it("renders nothing until it is opened, applies into the settings and closes on OK", async () => {
    await act(async () => root.render(<Studio initialOpen={false} />));
    expect(host.querySelector('[role="dialog"]')).toBeNull();
    await act(async () => button("open").click());
    expect(host.querySelector('[role="dialog"]')).not.toBeNull();
    await type(input("monitoring-settings-scatter-x-max"), "3000");
    await act(async () => button("Apply").click());
    expect(readout()).toBe("3000:1");
    expect(host.querySelector('[role="dialog"]')).not.toBeNull();
    await act(async () => button("OK").click());
    expect(readout()).toBe("3000:2");
    expect(host.querySelector('[role="dialog"]')).toBeNull();
  });

  it("Cancel leaves the settings alone, and the next open starts from the current settings", async () => {
    await act(async () => root.render(<Studio initialOpen />));
    await type(input("monitoring-settings-scatter-x-max"), "3000");
    await act(async () => button("Cancel").click());
    expect(readout()).toBe("1000:0");
    await act(async () => button("open").click());
    expect(input("monitoring-settings-scatter-x-max").value).toBe("1000.00");
  });
});
