import { describe, expect, it } from "vitest";
import {
  availablePanelWidth,
  clampHardwarePanelWidth,
  fitHardwarePanel,
  HW_PANEL_COMPACT_WIDTH,
  HW_PANEL_DEFAULT_WIDTH,
  HW_PANEL_MAX_WIDTH,
  HW_PANEL_MIN_WIDTH,
  panelBounds,
  parseCollapsed,
  parseHardwarePanelWidth,
  readStoredPanelLayout,
  resizeTarget,
  sanitizeHardwarePanelWidth,
  WORKSPACE_MIN_WIDTH,
} from "./hardwarePanelLayout";

// The numbers mirror Qt's tests/frontend/window_geometry_policy_test.cpp (fit, compact, hide, sanitise).
describe("stored width", () => {
  it("is the default when missing, non-numeric, non-positive or absurd", () => {
    for (const bad of [undefined, null, "", "  ", "abc", "NaN", "Infinity", -40, 0, "-1", "0", 100001, "999999", {}, [], true])
      expect(sanitizeHardwarePanelWidth(bad), String(bad)).toBe(HW_PANEL_DEFAULT_WIDTH);
    expect(parseHardwarePanelWidth("abc")).toBeNull();
  });
  it("is clamped to the range otherwise", () => {
    expect(sanitizeHardwarePanelWidth(120)).toBe(HW_PANEL_MIN_WIDTH);
    expect(sanitizeHardwarePanelWidth("1500")).toBe(HW_PANEL_MAX_WIDTH);
    expect(sanitizeHardwarePanelWidth(412.6)).toBe(413);
    expect(sanitizeHardwarePanelWidth("430")).toBe(430);
    expect(clampHardwarePanelWidth(1)).toBe(HW_PANEL_MIN_WIDTH);
  });
  it("reads the collapsed flag as the left sidebar does: only \"1\" is collapsed", () => {
    expect(parseCollapsed("1")).toBe(true);
    for (const other of ["0", "", "true", null, undefined]) expect(parseCollapsed(other)).toBe(false);
  });
  it("reads both from storage, and falls back to the defaults when storage throws or is absent", () => {
    const store = (values: Record<string, string>) => ({ getItem: (k: string) => values[k] ?? null });
    expect(readStoredPanelLayout(store({ "mib.hardwarePanel.collapsed": "1", "mib.hardwarePanel.width": "500" }))).toEqual({ collapsed: true, width: 500 });
    expect(readStoredPanelLayout(store({}))).toEqual({ collapsed: false, width: HW_PANEL_DEFAULT_WIDTH });
    expect(readStoredPanelLayout({ getItem: () => { throw new Error("blocked"); } })).toEqual({ collapsed: false, width: HW_PANEL_DEFAULT_WIDTH });
    expect(readStoredPanelLayout(undefined)).toEqual({ collapsed: false, width: HW_PANEL_DEFAULT_WIDTH });
  });
});

describe("fit beside the main area", () => {
  const fit = (viewportWidth: number, telemetryCollapsed = false, preferred = HW_PANEL_DEFAULT_WIDTH) =>
    fitHardwarePanel({ preferred, viewportWidth, telemetryCollapsed });

  it("gives the preferred width when there is room", () => {
    expect(fit(1600)).toMatchObject({ fits: true, compact: false, width: 360 });
    expect(fit(1366)).toMatchObject({ fits: true, compact: false, width: 360 });
  });
  it("narrows to what is left, down to the compact width", () => {
    const f = fit(1250); // 1250 - 264 (telemetry sidebar + strip) - 28 (own edge) - 640 (main) = 318
    expect(f).toMatchObject({ fits: true, compact: true, width: 318, available: 318 });
    expect(fit(1172)).toMatchObject({ fits: true, compact: true, width: HW_PANEL_COMPACT_WIDTH });
  });
  it("does not fit when even the compact width would push the main area below its minimum", () => {
    expect(fit(1171)).toMatchObject({ fits: false, width: 0 });
    expect(fit(1024)).toMatchObject({ fits: false, width: 0, available: 1024 - 264 - 28 - WORKSPACE_MIN_WIDTH });
  });
  it("gets the telemetry sidebar's room back when that is collapsed", () => {
    expect(fit(1024, true)).toMatchObject({ fits: true, compact: true, width: 342 }); // 1024 - 14 - 28 - 640 = 342
    expect(fit(1100, true)).toMatchObject({ fits: true, compact: false, width: 360 }); // 418 left
    expect(availablePanelWidth(1000, true)).toBe(318);
    expect(availablePanelWidth(1000, false)).toBe(68);
    expect(availablePanelWidth(100, false)).toBe(0);
  });
  it("never leaves the main area narrower than Qt's workspace minimum (640 px), which fits the stage tab bar", () => {
    expect(WORKSPACE_MIN_WIDTH).toBe(640); // Qt: frontend::geometry::kWorkspaceMinWidth
    for (const telemetryCollapsed of [false, true]) {
      const left = telemetryCollapsed ? 14 : 264;
      for (let viewportWidth = 900; viewportWidth <= 1700; viewportWidth++) {
        const f = fit(viewportWidth, telemetryCollapsed);
        if (f.fits) expect(viewportWidth - left - 28 - f.width, `${viewportWidth} px`).toBeGreaterThanOrEqual(640);
      }
    }
    // The window widths where the Review tab was cut off at 560 px: the panel now hides instead.
    for (const viewportWidth of [1101, 1150, 1171]) expect(fit(viewportWidth).fits).toBe(false);
  });
  it("clamps an unsanitised preference before fitting", () => {
    expect(fit(2000, false, 5000).width).toBe(HW_PANEL_MAX_WIDTH);
    expect(fit(2000, false, 10).width).toBe(HW_PANEL_MIN_WIDTH);
  });
});

describe("resize bounds", () => {
  it("are [min, max] when there is room", () => {
    expect(panelBounds(1000)).toEqual({ min: HW_PANEL_MIN_WIDTH, max: HW_PANEL_MAX_WIDTH });
    expect(resizeTarget(100, 1000)).toBe(HW_PANEL_MIN_WIDTH);
    expect(resizeTarget(5000, 1000)).toBe(HW_PANEL_MAX_WIDTH);
    expect(resizeTarget(400.4, 1000)).toBe(400);
  });
  it("shrink to what the main area leaves", () => {
    expect(panelBounds(300)).toEqual({ min: HW_PANEL_MIN_WIDTH, max: 300 });
    expect(panelBounds(260)).toEqual({ min: 260, max: 260 });
    expect(resizeTarget(500, 300)).toBe(300);
  });
  it("hold the compact width in a window too narrow for the panel", () => {
    expect(panelBounds(0)).toEqual({ min: HW_PANEL_COMPACT_WIDTH, max: HW_PANEL_COMPACT_WIDTH });
    expect(resizeTarget(500, 100)).toBe(HW_PANEL_COMPACT_WIDTH);
  });
});
