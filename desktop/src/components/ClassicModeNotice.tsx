// The classic view after the workspace changed the camera mode (#716 A3): leaving the workspace
// never switches the camera, so the classic tab says which mode the camera is in, which one this
// tab uses, and offers the switch as an explicit button (the same applyCameraMode as a tab change).
import { Button, Notice, UiRoot } from "@mib/ui";
import type { ClassicModeNotice as Model } from "../cameraWindow";

export function ClassicModeNotice({ notice, busy, onApply }: { notice: Model; busy: boolean; onApply: () => void }) {
  return (
    <UiRoot className="classic-mode-notice">
      <Notice
        tone="warn"
        title={`Camera mode · ${notice.actual}, this tab uses ${notice.expected}`}
        data-testid="classic-mode-notice"
        actions={<Button variant="primary" disabled={busy || !!notice.blocked} title={notice.blocked || undefined} onClick={onApply}>
          {busy ? "Switching…" : notice.action}
        </Button>}
      >
        {notice.blocked
          ? `The workspace changed the camera mode. ${notice.action} is unavailable: ${notice.blocked}.`
          : "The workspace changed the camera mode. Leaving it switched nothing: press the button to use this tab's mode."}
      </Notice>
    </UiRoot>
  );
}
