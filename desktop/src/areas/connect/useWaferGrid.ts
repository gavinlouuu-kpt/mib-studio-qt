// Wafer Grid on the Overview (CONNECT-054, epic #863; bridge ABI 45). Qt parity: OverviewTab's
// "Wafer Grid" button, showEvent / hideEvent (decode only while the Overview is on screen),
// onToggleDotGrid and updateDotGridOverlay. The state the button reads is React state; the overlay
// the live canvas draws is a ref (a pose arrives at 4 Hz and must not re-render the whole view).
import { useCallback, useEffect, useRef, useState } from "react";
import { bridge } from "../../bridge";
import type { PreviewState } from "../preview/usePreview";
import type { MonitoringState } from "../monitoring/useMonitoring";
import type { ShellState } from "../shell/useShell";
import type { ConnectState } from "./useConnect";
import {
  applyDotGridPose, displayScale, drawWaferGrid, INITIAL_WAFER_GRID, nextSinceSequence, waferGridActive,
  waferGridUnavailableText, WAFER_GRID_POLL_MS, type WaferGridState,
} from "./waferGrid";

type Deps = Pick<ShellState, "ready" | "tab" | "caps" | "append"> & Pick<MonitoringState, "expActive">
  & Pick<ConnectState, "waferGridOverlayRef"> & Pick<PreviewState, "framePulls"> & { viewerOnly: boolean };

/** True while the window is shown (not hidden behind another one or minimised). */
function useWindowVisible(): boolean {
  const [visible, setVisible] = useState(() => typeof document === "undefined" || document.visibilityState !== "hidden");
  useEffect(() => {
    const onChange = () => setVisible(document.visibilityState !== "hidden");
    document.addEventListener("visibilitychange", onChange);
    return () => document.removeEventListener("visibilitychange", onChange);
  }, []);
  return visible;
}

export function useWaferGrid({ ready, tab, caps, append, expActive, waferGridOverlayRef, framePulls, viewerOnly }: Deps) {
  // PZ7035 Align has no host frame store to decode (its frames come from the PL bridge): no button there.
  const supported = caps.frame_buffer;
  const windowVisible = useWindowVisible();
  const active = waferGridActive({ ready, supported, tab, windowVisible, experimentActive: expActive });

  // What the poll copied: the full state in a ref (the overlay reads it), the part the button needs in React state.
  const stateRef = useRef<WaferGridState>(INITIAL_WAFER_GRID);
  const [summary, setSummary] = useState({ enabled: false, available: true, designs: 0 });
  const [error, setError] = useState("");

  const adopt = useCallback((next: WaferGridState) => {
    stateRef.current = next;
    setSummary((s) => (s.enabled === next.enabled && s.available === next.available && s.designs === next.designs
      ? s : { enabled: next.enabled, available: next.available, designs: next.designs }));
  }, []);

  // Decode only while the Overview is on screen. The bridge starts paused, so a browser that never shows the
  // Overview costs nothing; a viewer (not the controlling client) cannot pause a service it does not control.
  useEffect(() => {
    if (!ready || !supported || viewerOnly) return;
    void bridge.setDotGridPaused(!active).catch(() => undefined);
  }, [ready, supported, viewerOnly, active]);
  const pausedOnExit = useRef<() => void>(() => undefined);
  pausedOnExit.current = () => { if (ready && supported && !viewerOnly) void bridge.setDotGridPaused(true).catch(() => undefined); };
  useEffect(() => () => pausedOnExit.current(), []);

  // Poll the pose at about 4 Hz while enabled and shown; one read on arrival learns whether something else
  // (config, another client, the previous page load) already switched the service on.
  useEffect(() => {
    if (!active) { waferGridOverlayRef.current = null; return; }
    let live = true, busy = false;
    const poll = async () => {
      if (busy) return;
      busy = true;
      try {
        const pose = await bridge.fetchDotGridPose(nextSinceSequence(stateRef.current));
        if (live) adopt(applyDotGridPose(stateRef.current, pose));
      } catch { /* the next poll tries again */ } finally { busy = false; }
    };
    void poll();
    const id = window.setInterval(() => { if (stateRef.current.enabled) void poll(); }, WAFER_GRID_POLL_MS);
    // The live canvas draws this after the frame and the window box (usePreview draw()).
    waferGridOverlayRef.current = (ctx) => {
      const { enabled, view } = stateRef.current;
      if (enabled && view) drawWaferGrid(ctx, view, displayScale(ctx.canvas));
    };
    return () => { live = false; window.clearInterval(id); waferGridOverlayRef.current = null; };
  }, [active, adopt, waferGridOverlayRef]);

  // Repaint the last frame after a toggle: a stopped camera sends no frame that would draw or clear the overlay.
  const repaint = useCallback(() => { framePulls.current.request("live", bridge.fetchFrame, false); }, [framePulls]);

  const onToggleWaferGrid = useCallback(async () => {
    const want = !stateRef.current.enabled;
    try {
      const result = await bridge.setDotGridEnabled(want);
      if (!result.ok) {
        // Qt: the "Dot-grid localization is not available: <reason>" dialog.
        const text = waferGridUnavailableText(result.message);
        setError(text);
        append(text);
        return;
      }
      setError("");
      append(`Wafer Grid ${want ? "on" : "off"}`);
      const pose = await bridge.fetchDotGridPose(0);
      adopt(applyDotGridPose({ ...INITIAL_WAFER_GRID }, pose));
      repaint();
    } catch (e) {
      const text = waferGridUnavailableText(String(e).replace(/^Error: /, ""));
      setError(text);
      append(text);
    }
  }, [adopt, append, repaint]);

  return { waferGrid: summary, waferGridSupported: supported, waferGridError: error, onToggleWaferGrid };
}
export type WaferGridModel = ReturnType<typeof useWaferGrid>;
