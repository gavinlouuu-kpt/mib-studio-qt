import type { ReactNode } from "react";
import { useControlRole } from "../transport/useControlRole";

// The classic view for a viewer (#763 F19). In a browser one client controls
// the instrument; the server refuses every mutating command of the others
// with VIEWER_ONLY. The classic view left those buttons enabled, so a press
// only logged the refusal. ControlBanner already tells a viewer to take
// control; these disable the controls that need it, as the workspace does
// with `controlHeld`. The desktop shell has no session and always controls,
// so nothing changes there.

/** Why a control is disabled for a viewer. */
export const VIEWER_ONLY_REASON = "Another client controls this instrument: take control (the banner at the top) to operate.";

/** True while another client controls the instrument (browser only). */
export function useViewerOnly(): boolean {
  return useControlRole().viewerOnly;
}

/** A disabled reason that puts the viewer's first: `reason` otherwise. */
export function viewerReason(viewerOnly: boolean, reason?: string): string | undefined {
  return viewerOnly ? VIEWER_ONLY_REASON : reason;
}

/** Disables every form control inside it for a viewer: a `fieldset` with
 * `display: contents` (no box, so toolbars keep their layout), whose
 * `disabled` the browser applies to each descendant button, input, select
 * and textarea. Wrap only controls that send a control command; navigation,
 * view toggles and reads stay outside. Not inside a `<p>` or `<label>`
 * (a fieldset is flow content). */
export function ViewerGate({ children }: { children: ReactNode }) {
  const viewerOnly = useViewerOnly();
  return (
    <fieldset className="viewer-gate" disabled={viewerOnly} title={viewerOnly ? VIEWER_ONLY_REASON : undefined} data-viewer-gate={viewerOnly ? "locked" : "open"}>
      {children}
    </fieldset>
  );
}
