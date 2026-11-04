// Hardware (epic #868): Operator vs Service/Commissioning mode and the one-shot actuation arm, the
// trigger/pulse generator test state, and the autofocus and pump snapshots. The device panels
// themselves are components/HardwareControls.tsx and components/StageControls.tsx. Moved from
// App.tsx without change (#869 wave 0); studio/useStudioController.ts calls these hooks in
// App.tsx's original effect order.
import { useCallback, useEffect, useRef, useState } from "react";
import { bridge, type AutofocusStatus, type PumpStatus, type TriggerStatus } from "../../bridge";
import {
  canActuate,
  canStartPeriodic,
  canStopPeriodic,
  DEFAULT_MODE,
  type OperatingMode,
} from "../../commissioning";
import { confirm } from "../../transport/dialogs";
import type { MonitoringState } from "../monitoring/useMonitoring";
import type { ShellState } from "../shell/useShell";
import {
  fitHardwarePanel,
  HW_PANEL_COLLAPSED_KEY,
  HW_PANEL_COLLAPSED_PREF,
  HW_PANEL_COMPACT_WIDTH,
  HW_PANEL_DEFAULT_WIDTH,
  HW_PANEL_NARROW_NOTICE,
  HW_PANEL_WIDTH_KEY,
  HW_PANEL_WIDTH_PREF,
  panelBounds,
  parseHardwarePanelWidth,
  readStoredPanelLayout,
  resizeTarget,
} from "./hardwarePanelLayout";

// localStorage can throw or be absent (blocked site data, tests): every access is guarded.
function panelStorage(): Storage | undefined {
  try { return window.localStorage; } catch { return undefined; }
}

export function useHardwareState() {
  const [trigStatus, setTrigStatus] = useState<TriggerStatus | null>(null);
  const [pulseUs, setPulseUs] = useState("1");
  const [periodicMs, setPeriodicMs] = useState("1000");
  // UX-9: routine Operator vs Service/Commissioning mode. Every session starts
  // in Operator mode; `triggerArmed` is a one-shot safety arm for actuation.
  const [operatingMode, setOperatingMode] = useState<OperatingMode>(DEFAULT_MODE);
  const [triggerArmed, setTriggerArmed] = useState(false);

  // Autofocus status (schema v11, BE-8).
  const [afStatus, setAfStatus] = useState<AutofocusStatus | null>(null);
  // UX-3 preflight: pump/trigger snapshots polled while on the Preflight stage.
  const [samplePump, setSamplePump] = useState<PumpStatus | null>(null);
  const [sheathPump, setSheathPump] = useState<PumpStatus | null>(null);
  // HW-018 / HW-042: the right-hand hardware panel. `hwPanelOpen` is the operator's intent (persisted);
  // whether the panel is shown also depends on the window width (useHardwarePanel). The width is the
  // preferred width, which the window may narrow. `hwNarrowOverride` = shown by hand although the window is narrow.
  const [hwPanelOpen, setHwPanelOpen] = useState(() => !readStoredPanelLayout(panelStorage()).collapsed);
  const [hwPanelPreferredWidth, setHwPanelPreferredWidth] = useState(() => readStoredPanelLayout(panelStorage()).width);
  const [hwViewportWidth, setHwViewportWidth] = useState(() => (typeof window === "undefined" ? 1280 : window.innerWidth));
  const [hwNarrowOverride, setHwNarrowOverride] = useState(false);
  return {
    trigStatus, setTrigStatus, pulseUs, setPulseUs, periodicMs, setPeriodicMs, operatingMode, setOperatingMode,
    triggerArmed, setTriggerArmed, afStatus, setAfStatus, samplePump, setSamplePump, sheathPump, setSheathPump,
    hwPanelOpen, setHwPanelOpen, hwPanelPreferredWidth, setHwPanelPreferredWidth, hwViewportWidth, setHwViewportWidth,
    hwNarrowOverride, setHwNarrowOverride,
  };
}
export type HardwareState = ReturnType<typeof useHardwareState>;

type ServiceModeDeps = Pick<ShellState, "append" | "instrumentModes">
  & Pick<HardwareState, "operatingMode" | "setOperatingMode" | "triggerArmed" | "setTriggerArmed" | "trigStatus">
  & Pick<MonitoringState, "expActive">;

// UX-9 Operator vs Service/Commissioning mode and the actuation gates of the trigger tests.
export function useServiceMode({
  append, instrumentModes, operatingMode, setOperatingMode, triggerArmed, setTriggerArmed, trigStatus, expActive,
}: ServiceModeDeps) {
  // ---- UX-9 Operator vs Service/Commissioning mode (issue #313) ----
  // Leaving Service mode, or an experiment going active, disarms actuation.
  useEffect(() => {
    if (operatingMode !== "service" || expActive) setTriggerArmed(false);
  }, [operatingMode, expActive]);

  const enterMode = async (next: OperatingMode) => {
    if (next === "service") {
      const ok = await confirm(
        "Enter Service / Commissioning mode?\n\nThis exposes hardware-actuating controls (trigger tests). Use only for bring-up and diagnostics.",
      );
      if (!ok) return;
      append("entered Service / Commissioning mode");
    } else {
      append("returned to Operator mode");
    }
    setOperatingMode(next);
    if (next !== "service") setTriggerArmed(false);
    // The backend refuses raw LED values outside Service mode (#501 P1).
    if (instrumentModes) void bridge.setServiceMode(next === "service").catch(() => {});
  };

  const actuateCheck = canActuate({
    mode: operatingMode,
    experimentActive: expActive,
    triggerAttached: trigStatus?.camera_attached ?? false,
    armed: triggerArmed,
  });
  const startPeriodicCheck = canStartPeriodic({
    mode: operatingMode,
    experimentActive: expActive,
    triggerAttached: trigStatus?.camera_attached ?? false,
    armed: triggerArmed,
  });
  const stopPeriodicCheck = canStopPeriodic({ periodicActive: trigStatus?.periodic_active ?? false });
  return { enterMode, actuateCheck, startPeriodicCheck, stopPeriodicCheck };
}

// ---- HW-018 / HW-042: the right-hand hardware panel (pumps, autofocus, pulse generator, Z stage) ----

// The shell preferences document is written whole (bridge.setPreferences), so the writes of this panel are
// queued one after the other: two quick changes must not read the same document and lose one.
let preferenceWrites: Promise<unknown> = Promise.resolve();
function savePanelPreference(patch: Record<string, unknown>) {
  preferenceWrites = preferenceWrites
    .then(() => bridge.getPreferences())
    .then((prefs) => bridge.setPreferences({ ...prefs, ...patch }))
    .catch(() => {});
}

/** Width writes to the preferences file wait this long after the last drag step or key press (Qt: 300 ms). */
export const HW_PANEL_PERSIST_MS = 300;

type PanelDeps = Pick<ShellState, "sidebarCollapsed">
  & Pick<HardwareState, "hwPanelOpen" | "setHwPanelOpen" | "hwPanelPreferredWidth" | "setHwPanelPreferredWidth"
    | "hwViewportWidth" | "setHwViewportWidth" | "hwNarrowOverride" | "setHwNarrowOverride">;

// Visibility, width, persistence and narrow-window handling of the hardware panel, as Qt's hardware sidebar
// (MainWindow::applySidebarLayout): the preferred width is kept; when the main area would drop below its
// minimum the panel narrows to its compact width, and below that it hides with a status notice and comes back
// by itself when space returns. Hiding for space is never persisted; hiding by hand is.
export function useHardwarePanel({
  sidebarCollapsed, hwPanelOpen, setHwPanelOpen, hwPanelPreferredWidth, setHwPanelPreferredWidth,
  hwViewportWidth, setHwViewportWidth, hwNarrowOverride, setHwNarrowOverride,
}: PanelDeps) {
  const fit = fitHardwarePanel({ preferred: hwPanelPreferredWidth, viewportWidth: hwViewportWidth, telemetryCollapsed: sidebarCollapsed });
  const narrow = !fit.fits;
  const shownByHand = narrow && hwNarrowOverride;
  const hardwarePanelVisible = hwPanelOpen && (fit.fits || shownByHand);
  const hardwarePanelHiddenForSpace = hwPanelOpen && narrow && !hwNarrowOverride;
  // A hand-shown panel in a narrow window takes the compact width, whatever the main area keeps.
  const hardwarePanelWidth = fit.fits ? fit.width : HW_PANEL_COMPACT_WIDTH;
  const hardwarePanelBounds = panelBounds(fit.available);

  // Window width (Qt: resizeEvent -> applySidebarLayout).
  useEffect(() => {
    const onResize = () => setHwViewportWidth(window.innerWidth);
    window.addEventListener("resize", onResize);
    onResize();
    return () => window.removeEventListener("resize", onResize);
  }, []);

  // A manual show in a narrow window lasts until the window has room again.
  useEffect(() => {
    if (!narrow) setHwNarrowOverride(false);
  }, [narrow]);

  // Restore the durable copy at boot (BE-9), as the left sidebar does: it survives webview-storage clearing.
  useEffect(() => {
    bridge
      .getPreferences()
      .then((prefs) => {
        const storage = panelStorage();
        if (typeof prefs[HW_PANEL_COLLAPSED_PREF] === "boolean") {
          setHwPanelOpen(!prefs[HW_PANEL_COLLAPSED_PREF]);
          try { storage?.setItem(HW_PANEL_COLLAPSED_KEY, prefs[HW_PANEL_COLLAPSED_PREF] ? "1" : "0"); } catch { /* unavailable */ }
        }
        const width = parseHardwarePanelWidth(prefs[HW_PANEL_WIDTH_PREF]);
        if (width !== null) {
          setHwPanelPreferredWidth(width);
          try { storage?.setItem(HW_PANEL_WIDTH_KEY, String(width)); } catch { /* unavailable */ }
        }
      })
      .catch(() => {});
  }, []);

  const widthTimer = useRef<number | undefined>(undefined);
  useEffect(() => () => window.clearTimeout(widthTimer.current), []);

  const setHardwarePanelVisible = useCallback((show: boolean) => {
    setHwPanelOpen(show);
    // Showing by hand in a window too narrow for it is allowed; the main area gives way.
    setHwNarrowOverride(show && narrow);
    try { panelStorage()?.setItem(HW_PANEL_COLLAPSED_KEY, show ? "0" : "1"); } catch { /* unavailable */ }
    savePanelPreference({ [HW_PANEL_COLLAPSED_PREF]: !show });
  }, [narrow]);
  // For the shell's menu item and Ctrl+Shift+H (SHELL-012, #862).
  const toggleHardwarePanel = useCallback(() => setHardwarePanelVisible(!hardwarePanelVisible), [setHardwarePanelVisible, hardwarePanelVisible]);

  // `commit` ends a drag step or a key press: it stores the width (the preferences file after a short pause).
  const resizeHardwarePanel = useCallback((requested: number, commit: boolean) => {
    const next = resizeTarget(requested, fit.available);
    setHwPanelPreferredWidth(next);
    if (!commit) return;
    try { panelStorage()?.setItem(HW_PANEL_WIDTH_KEY, String(next)); } catch { /* unavailable */ }
    window.clearTimeout(widthTimer.current);
    widthTimer.current = window.setTimeout(() => savePanelPreference({ [HW_PANEL_WIDTH_PREF]: next }), HW_PANEL_PERSIST_MS);
  }, [fit.available]);
  const resetHardwarePanelWidth = useCallback(() => resizeHardwarePanel(HW_PANEL_DEFAULT_WIDTH, true), [resizeHardwarePanel]);

  return {
    /** The operator's intent: false only after hiding it by hand. */
    hardwarePanelOpen: hwPanelOpen,
    /** Whether the panel is on screen now (open, and the window has room or the operator showed it anyway). */
    hardwarePanelVisible,
    hardwarePanelHiddenForSpace,
    /** Width of the panel body in px. */
    hardwarePanelWidth,
    hardwarePanelBounds,
    /** Status-bar text while the panel is hidden for space; "" otherwise. */
    hardwarePanelNotice: hardwarePanelHiddenForSpace ? HW_PANEL_NARROW_NOTICE : "",
    toggleHardwarePanel,
    setHardwarePanelVisible,
    resizeHardwarePanel,
    resetHardwarePanelWidth,
  };
}
