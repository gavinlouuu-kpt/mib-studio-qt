// Hardware (epic #868, HW-018 / HW-042): the geometry and the persisted form of the right-hand hardware
// panel, as pure functions so the layout rules are unit-tested without a DOM. Qt's counterpart is
// src/frontend/utils/WindowGeometryPolicy.{h,cpp} (fitSidebarWidth, sanitizeSidebarPreferredWidth) and
// MainWindow::applySidebarLayout; the numbers are the web equivalents (the device forms need more room
// than Qt's compact boxes).

/** Preferred width when nothing was stored (Qt: 300 px). */
export const HW_PANEL_DEFAULT_WIDTH = 360;
/** Floor of the preferred width: a drag or a stored value below it is raised to it (Qt: 200 px). */
export const HW_PANEL_MIN_WIDTH = 280;
/** The narrowest the panel is shown when space is short; below it the panel hides (Qt: 160 px). */
export const HW_PANEL_COMPACT_WIDTH = 240;
/** Ceiling of the preferred width (Qt: 1000 px). */
export const HW_PANEL_MAX_WIDTH = 720;
/** The main area (tabs and stage content) keeps at least this width (Qt: kWorkspaceMinWidth, 640 px). */
export const WORKSPACE_MIN_WIDTH = 560;
/** Telemetry sidebar (layout.css `.sidebar`) and its toggle strip, which sit left of the main area. */
export const TELEMETRY_SIDEBAR_WIDTH = 250;
export const TELEMETRY_TOGGLE_WIDTH = 14;
/** The hardware panel's own edge: the toggle tab and the drag handle (hardware-panel.css). */
export const HW_PANEL_TOGGLE_WIDTH = 22;
export const HW_PANEL_HANDLE_WIDTH = 6;
export const HW_PANEL_EDGE_WIDTH = HW_PANEL_TOGGLE_WIDTH + HW_PANEL_HANDLE_WIDTH;

/** Keyboard step of the resize handle, in px (Shift = 4x). */
export const HW_PANEL_KEY_STEP = 16;

/** Qt's wording (MainWindow::applySidebarLayout status-bar message). */
export const HW_PANEL_NARROW_NOTICE = "Window too narrow for the hardware panel; enlarge the window to show it.";

/** localStorage keys: the left sidebar's `mib.sidebar.collapsed` is the model ("1" = collapsed). */
export const HW_PANEL_COLLAPSED_KEY = "mib.hardwarePanel.collapsed";
export const HW_PANEL_WIDTH_KEY = "mib.hardwarePanel.width";
/** Keys in the shell preferences document (bridge.getPreferences / setPreferences, BE-9). */
export const HW_PANEL_COLLAPSED_PREF = "hardwarePanelCollapsed";
export const HW_PANEL_WIDTH_PREF = "hardwarePanelWidth";

export function clampHardwarePanelWidth(width: number): number {
  return Math.min(HW_PANEL_MAX_WIDTH, Math.max(HW_PANEL_MIN_WIDTH, Math.round(width)));
}

/** A stored width as a clamped number, or null when it is missing, non-numeric, non-positive or absurd. Accepts a
 * number or a numeric string. */
export function parseHardwarePanelWidth(stored: unknown): number | null {
  const value = typeof stored === "string" && stored.trim() !== "" ? Number(stored) : stored;
  if (typeof value !== "number" || !Number.isFinite(value) || value <= 0 || value > 100000) return null;
  return clampHardwarePanelWidth(value);
}

/** A stored width, or the default when unusable (Qt: sanitizeSidebarPreferredWidth). */
export function sanitizeHardwarePanelWidth(stored: unknown): number {
  return parseHardwarePanelWidth(stored) ?? HW_PANEL_DEFAULT_WIDTH;
}

/** A stored collapsed flag from localStorage ("1"/"0"); anything else is "not collapsed" (the Qt default is visible). */
export function parseCollapsed(stored: string | null | undefined): boolean {
  return stored === "1";
}

export function readStoredPanelLayout(storage: Pick<Storage, "getItem"> | undefined): { collapsed: boolean; width: number } {
  try {
    return {
      collapsed: parseCollapsed(storage?.getItem(HW_PANEL_COLLAPSED_KEY)),
      width: sanitizeHardwarePanelWidth(storage?.getItem(HW_PANEL_WIDTH_KEY)),
    };
  } catch {
    // Storage can throw (blocked site data, private windows): the defaults apply.
    return { collapsed: false, width: HW_PANEL_DEFAULT_WIDTH };
  }
}

export interface PanelFitInput {
  /** The operator's preferred width (already sanitised). */
  preferred: number;
  /** Width of the window's client area. */
  viewportWidth: number;
  /** Whether the telemetry sidebar is collapsed (it then gives its 250 px back). */
  telemetryCollapsed: boolean;
}

export interface PanelFit {
  /** True when at least the compact panel fits beside the main area; false = hide for space. */
  fits: boolean;
  /** True when the panel is narrower than the preferred width. */
  compact: boolean;
  /** Width of the panel body to render (0 when it does not fit). */
  width: number;
  /** Most width the panel body could take now, for the resize handle (>= 0). */
  available: number;
}

/** Space left for the panel body beside the telemetry sidebar, the main area's minimum and the panel's own edge. */
export function availablePanelWidth(viewportWidth: number, telemetryCollapsed: boolean): number {
  const left = TELEMETRY_TOGGLE_WIDTH + (telemetryCollapsed ? 0 : TELEMETRY_SIDEBAR_WIDTH);
  return Math.max(0, Math.floor(viewportWidth) - left - WORKSPACE_MIN_WIDTH - HW_PANEL_EDGE_WIDTH);
}

/** Qt's fitSidebarWidth: the preferred width when it fits, else whatever is left down to the compact width,
 * else it does not fit and the panel hides (the preference is kept for when space returns). */
export function fitHardwarePanel({ preferred, viewportWidth, telemetryCollapsed }: PanelFitInput): PanelFit {
  const wanted = clampHardwarePanelWidth(preferred);
  const available = availablePanelWidth(viewportWidth, telemetryCollapsed);
  if (available >= wanted) return { fits: true, compact: false, width: wanted, available };
  if (available >= HW_PANEL_COMPACT_WIDTH) return { fits: true, compact: true, width: available, available };
  return { fits: false, compact: true, width: 0, available };
}

/** Smallest and largest width the resize handle can set: [min, max] clamped to the space the main area leaves. A
 * window so narrow that even the compact panel does not fit leaves the compact width (a manual show is allowed there). */
export function panelBounds(available: number): { min: number; max: number } {
  const max = Math.max(HW_PANEL_COMPACT_WIDTH, Math.min(HW_PANEL_MAX_WIDTH, available));
  return { min: Math.min(HW_PANEL_MIN_WIDTH, max), max };
}

/** Width a drag or key press asks for, kept within the bounds. */
export function resizeTarget(requested: number, available: number): number {
  const { min, max } = panelBounds(available);
  return Math.min(max, Math.max(min, Math.round(requested)));
}
