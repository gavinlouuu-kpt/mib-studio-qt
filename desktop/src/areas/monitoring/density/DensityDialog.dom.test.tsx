// @vitest-environment jsdom
// The Monitoring Settings dialog's Density (KDE) group and the toggle for a viewer (epic #866 PR E2: MON-061, 083..086).
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

let state: { clientId?: number; controllerId?: number | null } = {};
const listeners = new Set<(s: typeof state) => void>();
vi.mock("../../../transport/index", () => ({
  transport: { kind: "ws", invoke: vi.fn(), session: { get: () => state, subscribe: (l: (s: typeof state) => void) => { listeners.add(l); return () => listeners.delete(l); } } },
}));
import { VIEWER_ONLY_REASON } from "../../../components/ViewerGate";
import { MonitoringSettingsDialogView } from "../MonitoringSettingsDialog";
import { applyMonitoringSettings, DEFAULT_MONITORING_SETTINGS } from "../monitoringSettings";
import { DensityToggle } from "./DensityToggle";
import type { DensityReferenceControlsModel } from "./useMonitoringDensity";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let host: HTMLDivElement, root: Root;
beforeEach(() => {
  state = {};
  listeners.clear();
  host = document.createElement("div");
  document.body.append(host);
  root = createRoot(host);
});
afterEach(async () => {
  await act(async () => root.unmount());
  host.remove();
});

const q = (tour: string) => host.querySelector<HTMLElement>(`[data-tour="${tour}"]`);
const setState = async (next: typeof state) => { state = next; await act(async () => { listeners.forEach((l) => l(state)); }); };
const type = (el: HTMLInputElement, value: string) =>
  act(async () => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(el, value);
    el.dispatchEvent(new Event("input", { bubbles: true }));
  });

const controls = (over: Partial<DensityReferenceControlsModel> = {}): DensityReferenceControlsModel => ({
  label: null, canPin: true, canClear: false, onPin: vi.fn(), onFromFile: vi.fn(async () => undefined), onClear: vi.fn(), ...over,
});
function dialog(reference?: DensityReferenceControlsModel) {
  const onApply = vi.fn((v) => applyMonitoringSettings(DEFAULT_MONITORING_SETTINGS, v).ignored);
  return { onApply, element: <MonitoringSettingsDialogView settings={DEFAULT_MONITORING_SETTINGS} onApply={onApply} onClose={vi.fn()} reference={reference} /> };
}

describe("the Density (KDE) group of the dialog", () => {
  it("has the three Qt fields first with their tour anchors, and the reference buttons under them", async () => {
    await act(async () => root.render(dialog(controls()).element));
    const group = q("monitoring-settings-density")!;
    expect(group.querySelector("legend")!.textContent).toBe("Density (KDE)");
    expect(Array.from(group.querySelectorAll(".mon-settings-label")).map((l) => l.textContent)).toEqual(["KDE bandwidth factor", "KDE update interval", "Core contour", "Reference contour"]);
    for (const tour of ["monitoring-settings-kde-bandwidth", "monitoring-settings-kde-interval", "monitoring-settings-kde-core", "monitoring-settings-kde-pin", "monitoring-settings-kde-from-file", "monitoring-settings-kde-clear"])
      expect(group.querySelector(`[data-tour="${tour}"]`), tour).not.toBeNull();
    expect(Array.from(group.querySelectorAll("button")).map((b) => b.textContent)).toEqual(["Pin current", "From file…", "Clear"]);
    expect(Array.from(group.querySelectorAll(".mon-settings-suffix")).map((s) => s.textContent)).toEqual(["ms", "%"]);
  });

  it("applies the density numbers with the others: factor 2 decimals, interval and core whole numbers, clamped to Qt's ranges", async () => {
    const d = dialog(controls());
    await act(async () => root.render(d.element));
    await type(q("monitoring-settings-kde-bandwidth") as HTMLInputElement, "7.777");
    await type(q("monitoring-settings-kde-interval") as HTMLInputElement, "100");
    await type(q("monitoring-settings-kde-core") as HTMLInputElement, "42.4");
    await act(async () => Array.from(host.querySelectorAll("button")).find((b) => b.textContent === "Apply")!.click());
    expect(d.onApply.mock.calls[0][0]).toMatchObject({ kdeBandwidthFactor: 5, kdeIntervalMs: 500, kdeCorePercent: 42 });
    expect((q("monitoring-settings-kde-bandwidth") as HTMLInputElement).value).toBe("5.00");
    expect((q("monitoring-settings-kde-interval") as HTMLInputElement).value).toBe("500");
    expect((q("monitoring-settings-kde-core") as HTMLInputElement).value).toBe("42");
    const applied = applyMonitoringSettings(DEFAULT_MONITORING_SETTINGS, d.onApply.mock.calls[0][0]).settings;
    expect(applied).toMatchObject({ kdeBandwidthFactor: 5, kdeIntervalMs: 500, kdeCorePercent: 42, scatterEpoch: 1 });
  });

  it("has no reference row when the shell gave none", async () => {
    await act(async () => root.render(dialog().element));
    expect(q("monitoring-settings-kde-reference")).toBeNull();
    expect(q("monitoring-settings-kde-bandwidth")).not.toBeNull();
  });
});

describe("the reference buttons", () => {
  const button = (tour: string) => q(tour) as HTMLButtonElement;

  it("enable Pin current only with a contour to keep, Clear only with a reference, and From file… unless blocked", async () => {
    await act(async () => root.render(dialog(controls({ canPin: false, canClear: false })).element));
    expect(button("monitoring-settings-kde-pin").disabled).toBe(true);
    expect(button("monitoring-settings-kde-clear").disabled).toBe(true);
    expect(button("monitoring-settings-kde-from-file").disabled).toBe(false);
    await act(async () => root.render(dialog(controls({ canPin: true, canClear: true, label: "run7.h5, full-run 90%", fromFileBlocked: "not while a run is active" })).element));
    expect(button("monitoring-settings-kde-pin").disabled).toBe(false);
    expect(button("monitoring-settings-kde-clear").disabled).toBe(false);
    expect(button("monitoring-settings-kde-from-file").disabled).toBe(true);
    expect(button("monitoring-settings-kde-from-file").title).toBe("not while a run is active");
    expect(q("monitoring-settings-kde-reference-label")!.textContent).toBe("Reference: run7.h5, full-run 90%");
  });

  it("act at once, without applying or closing the dialog", async () => {
    const c = controls({ canClear: true });
    const d = dialog(c);
    await act(async () => root.render(d.element));
    await act(async () => button("monitoring-settings-kde-pin").click());
    await act(async () => button("monitoring-settings-kde-clear").click());
    await act(async () => button("monitoring-settings-kde-from-file").click());
    expect(c.onPin).toHaveBeenCalledOnce();
    expect(c.onClear).toHaveBeenCalledOnce();
    expect(c.onFromFile).toHaveBeenCalledOnce();
    expect(d.onApply).not.toHaveBeenCalled();
  });

  it("keeps From file… pressed until the picker and the read are over", async () => {
    let finish!: () => void;
    const c = controls({ onFromFile: vi.fn(() => new Promise<void>((resolve) => { finish = resolve; })) });
    await act(async () => root.render(dialog(c).element));
    await act(async () => button("monitoring-settings-kde-from-file").click());
    expect(button("monitoring-settings-kde-from-file").disabled).toBe(true);
    await act(async () => button("monitoring-settings-kde-from-file").click());
    expect(c.onFromFile).toHaveBeenCalledOnce();
    await act(async () => finish());
    expect(button("monitoring-settings-kde-from-file").disabled).toBe(false);
  });

  it("are not behind a ViewerGate: none of them sends a control command", async () => {
    await act(async () => root.render(dialog(controls({ canClear: true })).element));
    await setState({ clientId: 7, controllerId: 3 });
    for (const tour of ["monitoring-settings-kde-pin", "monitoring-settings-kde-from-file", "monitoring-settings-kde-clear"]) expect(button(tour).disabled, tour).toBe(false);
  });
});

describe("the toggle for a viewer", () => {
  const model = { onChange: vi.fn(), tooltip: "Density (KDE) over 2 points", notice: "" };
  it("is disabled with the reason while another client controls the instrument, and follows a handover", async () => {
    await act(async () => root.render(<DensityToggle on={false} toggle={model} />));
    await setState({ clientId: 7, controllerId: 3 });
    const box = q("monitoring-density-toggle") as HTMLInputElement;
    expect(box.matches(":disabled")).toBe(true);
    expect(box.closest("label")!.title).toBe(VIEWER_ONLY_REASON);
    await setState({ clientId: 7, controllerId: 7 });
    expect(box.matches(":disabled")).toBe(false);
    expect(box.closest("label")!.title).toBe("Density (KDE) over 2 points");
  });
});
